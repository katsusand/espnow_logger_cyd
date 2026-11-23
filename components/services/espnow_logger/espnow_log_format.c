#include "espnow_log_format.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

const char ESPNOW_LOG_CSV_HEADER[] = "utc_iso8601,ts_ms,src_mac,len,payload_hex\n";

static const char *const s_prefix[ESPNOW_LOG_FILE_COUNT] = {"sensor_", "espnow_"};
static const char *const s_suffix[ESPNOW_LOG_FILE_COUNT] = {".bin", ".csv"};

void espnow_log_format_mac(const uint8_t *mac, char out[ESPNOW_LOG_MAC_STR_LEN])
{
    if (mac == NULL) {
        (void)snprintf(out, ESPNOW_LOG_MAC_STR_LEN, "??:??:??:??:??:??");
        return;
    }
    (void)snprintf(out, ESPNOW_LOG_MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void espnow_log_format_hex(const uint8_t *payload, size_t len, char *out, size_t out_size)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t offset = 0;

    if (out == NULL || out_size == 0) {
        return;
    }
    if (payload == NULL || len == 0) {
        (void)snprintf(out, out_size, "-");
        return;
    }
    /* Each byte needs "XX" and a space between bytes; keep room for the NUL. */
    for (size_t i = 0; i < len && offset + 3 < out_size; ++i) {
        out[offset++] = digits[payload[i] >> 4];
        out[offset++] = digits[payload[i] & 0x0F];
        if (i + 1U < len) {
            out[offset++] = ' ';
        }
    }
    out[offset] = '\0';
}

void espnow_log_format_utc_iso8601(uint64_t epoch_ms, char *out, size_t out_size)
{
    struct tm tm_utc;
    time_t seconds = (time_t)(epoch_ms / 1000ULL);

    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (gmtime_r(&seconds, &tm_utc) == NULL) {
        return;
    }
    (void)snprintf(out,
                   out_size,
                   "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                   tm_utc.tm_year + 1900,
                   tm_utc.tm_mon + 1,
                   tm_utc.tm_mday,
                   tm_utc.tm_hour,
                   tm_utc.tm_min,
                   tm_utc.tm_sec,
                   (unsigned)(epoch_ms % 1000ULL));
}

int espnow_log_parse_mac(const char *text, uint8_t mac[6])
{
    unsigned int v[6];
    char tail = 0;

    if (text == NULL || mac == NULL) {
        return 0;
    }
    /* The trailing %c rejects "AA:..:FF:00". */
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) {
        return 0;
    }
    for (size_t i = 0; i < 6; ++i) {
        mac[i] = (uint8_t)v[i];
    }
    return 1;
}

size_t espnow_log_format_csv_line(
    const char *utc, uint64_t ts_ms, const uint8_t *src_mac, const uint8_t *payload, size_t len, char *out, size_t out_size)
{
    char mac[ESPNOW_LOG_MAC_STR_LEN];

    if (out == NULL) {
        return 0;
    }
    espnow_log_format_mac(src_mac, mac);
    int head = snprintf(out, out_size, "%s,%" PRIu64 ",%s,%u,", utc != NULL ? utc : "", ts_ms, mac, (unsigned)len);
    if (head <= 0 || (size_t)head >= out_size) {
        return 0;
    }
    /* The hex goes straight into `out`: this runs on the Wi-Fi task's small stack. */
    size_t hex_len = (payload == NULL || len == 0) ? 1U : len * 3U - 1U;
    if ((size_t)head + hex_len + 2U > out_size) {
        return 0;
    }
    espnow_log_format_hex(payload, len, out + head, out_size - (size_t)head - 1U);
    size_t total = (size_t)head + strlen(out + head);
    out[total] = '\n';
    out[total + 1U] = '\0';
    return total + 1U;
}

size_t espnow_log_file_path(espnow_log_file_kind_t kind, unsigned index, char *out, size_t out_size)
{
    if ((unsigned)kind >= ESPNOW_LOG_FILE_COUNT) {
        return 0;
    }
    int written = snprintf(out, out_size, ESPNOW_LOG_DIR "/%s%04u%s", s_prefix[kind], index, s_suffix[kind]);
    if (written <= 0 || (size_t)written >= out_size) {
        return 0;
    }
    return (size_t)written;
}

int espnow_log_file_parse_name(const char *name, espnow_log_file_kind_t *kind, unsigned *index)
{
    if (name == NULL) {
        return 0;
    }
    for (int k = 0; k < ESPNOW_LOG_FILE_COUNT; ++k) {
        size_t prefix_len = strlen(s_prefix[k]);
        if (strncasecmp(name, s_prefix[k], prefix_len) != 0) {
            continue;
        }
        const char *p = name + prefix_len;
        unsigned value = 0;
        size_t digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 9) {
            value = value * 10U + (unsigned)(*p - '0');
            ++p;
            ++digits;
        }
        if (digits == 0 || strcasecmp(p, s_suffix[k]) != 0) {
            continue;
        }
        if (kind != NULL) {
            *kind = (espnow_log_file_kind_t)k;
        }
        if (index != NULL) {
            *index = value;
        }
        return 1;
    }
    return 0;
}
