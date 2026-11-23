#pragma once

/*
 * Text and file-name rules of the SD log files (docs/sensor_log_format.md).
 *
 * English contract: pure C, no ESP-IDF headers, so host tests link the real
 * source. Every function writes a NUL-terminated string within `out_size`.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_LOG_MAC_STR_LEN 18U
#define ESPNOW_LOG_UTC_STR_LEN 32U
/* "xx " per byte for the largest ESP-NOW payload (250), and the CSV line around it. */
#define ESPNOW_LOG_HEX_MAX (250U * 3U + 1U)
#define ESPNOW_LOG_CSV_LINE_MAX (250U * 3U + 96U)

/* Directory on the card (relative to the mount point) that holds the log files. */
#define ESPNOW_LOG_DIR "logs"

typedef enum {
    ESPNOW_LOG_FILE_SENSOR = 0, /* sensor_NNNN.bin */
    ESPNOW_LOG_FILE_ESPNOW,     /* espnow_NNNN.csv */
    ESPNOW_LOG_FILE_COUNT,
} espnow_log_file_kind_t;

/* "AA:BB:CC:DD:EE:FF", or "??:??:??:??:??:??" for NULL. */
void espnow_log_format_mac(const uint8_t *mac, char out[ESPNOW_LOG_MAC_STR_LEN]);
/* "AA BB CC", or "-" when empty. Cut (never overrun) when `out` is short. */
void espnow_log_format_hex(const uint8_t *payload, size_t len, char *out, size_t out_size);
/* Unix epoch ms to "YYYY-MM-DDTHH:MM:SS.mmmZ". Empty string when it cannot be shown. */
void espnow_log_format_utc_iso8601(uint64_t epoch_ms, char *out, size_t out_size);

/* "AA:BB:..." to 6 bytes. false for anything else (including ""). */
int espnow_log_parse_mac(const char *text, uint8_t mac[6]);

/* First line of every espnow.csv, with its newline. */
extern const char ESPNOW_LOG_CSV_HEADER[];
/*
 * One espnow.csv row: utc_iso8601,ts_ms,src_mac,len,payload_hex. `utc` may be ""
 * (the clock is not known). Returns the length written, or 0 when it does not fit.
 */
size_t espnow_log_format_csv_line(
    const char *utc, uint64_t ts_ms, const uint8_t *src_mac, const uint8_t *payload, size_t len, char *out, size_t out_size);

/* "logs/sensor_0001.bin". Returns the length, or 0 when it does not fit. */
size_t espnow_log_file_path(espnow_log_file_kind_t kind, unsigned index, char *out, size_t out_size);
/*
 * Reads a directory entry's name ("sensor_0007.bin", case does not matter). Returns 1
 * and fills kind and index for one of ours, 0 for anything else.
 */
int espnow_log_file_parse_name(const char *name, espnow_log_file_kind_t *kind, unsigned *index);

#ifdef __cplusplus
}
#endif
