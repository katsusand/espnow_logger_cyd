/*
 * espnow.csv rows and the log file names.
 *
 * What is protected here: the CSV columns (a fixed header the PC side relies on),
 * a maximum-size payload still fitting the line buffer on the Wi-Fi task's stack,
 * and the file numbering rules of docs/sensor_log_format.md ("max + 1, never reuse").
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "espnow_log_format.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("  FAIL %s\n", what);
        failures++;
    } else {
        printf("  ok   %s\n", what);
    }
}

static void test_text(void)
{
    char out[ESPNOW_LOG_UTC_STR_LEN];
    char mac[ESPNOW_LOG_MAC_STR_LEN];
    char hex[16];
    const uint8_t bytes[] = {0x00, 0xAB, 0xFF};
    const uint8_t addr[] = {0xA0, 0xB1, 0xC2, 0x03, 0x04, 0x05};
    uint8_t parsed[6];

    espnow_log_format_utc_iso8601(1709208000123ULL, out, sizeof(out));
    check(strcmp(out, "2024-02-29T12:00:00.123Z") == 0, "UTC stamp with milliseconds");
    espnow_log_format_mac(addr, mac);
    check(strcmp(mac, "A0:B1:C2:03:04:05") == 0, "MAC is upper case, colon separated");
    espnow_log_format_mac(NULL, mac);
    check(strcmp(mac, "??:??:??:??:??:??") == 0, "no MAC");
    espnow_log_format_hex(bytes, 3, hex, sizeof(hex));
    check(strcmp(hex, "00 AB FF") == 0, "hex with spaces, none at the end");
    espnow_log_format_hex(NULL, 0, hex, sizeof(hex));
    check(strcmp(hex, "-") == 0, "empty payload is -");
    espnow_log_format_hex(bytes, 3, hex, 5);
    check(strlen(hex) < 5, "hex never overruns a short buffer");

    check(espnow_log_parse_mac("A0:b1:C2:03:04:05", parsed) && memcmp(parsed, addr, 6) == 0, "MAC parse accepts either case");
    check(!espnow_log_parse_mac("", parsed), "empty string is not a MAC");
    check(!espnow_log_parse_mac("A0:B1:C2:03:04", parsed), "five bytes is not a MAC");
    check(!espnow_log_parse_mac("A0:B1:C2:03:04:05:06", parsed), "seven bytes is not a MAC");
    check(!espnow_log_parse_mac("zz:B1:C2:03:04:05", parsed), "non-hex is not a MAC");
}

static void test_csv(void)
{
    char line[ESPNOW_LOG_CSV_LINE_MAX];
    const uint8_t src[] = {1, 2, 3, 4, 5, 6};
    const uint8_t payload[] = {0x5A, 0xA5, 0x02, 0x05, 0x01};
    uint8_t big[250];
    size_t n = espnow_log_format_csv_line("2024-02-29T12:00:00.123Z", 4567, src, payload, sizeof(payload), line, sizeof(line));

    check(strcmp(ESPNOW_LOG_CSV_HEADER, "utc_iso8601,ts_ms,src_mac,len,payload_hex\n") == 0, "header columns");
    check(strcmp(line, "2024-02-29T12:00:00.123Z,4567,01:02:03:04:05:06,5,5A A5 02 05 01\n") == 0, "row layout");
    check(n == strlen(line), "returned length counts the newline");

    n = espnow_log_format_csv_line("", 1, src, payload, sizeof(payload), line, sizeof(line));
    check(n > 0 && strncmp(line, ",1,01:02:03:04:05:06,5,", 23) == 0, "unknown clock leaves the first column empty");

    memset(big, 0xFF, sizeof(big));
    n = espnow_log_format_csv_line("2024-02-29T12:00:00.123Z", 18446744073709551615ULL, src, big, sizeof(big), line, sizeof(line));
    check(n > 0 && n == strlen(line) && line[n - 1] == '\n', "a 250 byte payload fits the line buffer");
    n = espnow_log_format_csv_line("", 1, src, big, sizeof(big), line, 100);
    check(n == 0, "a row that does not fit is refused, not cut");
}

static void test_names(void)
{
    char path[64];
    espnow_log_file_kind_t kind = ESPNOW_LOG_FILE_COUNT;
    unsigned index = 0;

    check(espnow_log_file_path(ESPNOW_LOG_FILE_SENSOR, 1, path, sizeof(path)) != 0 && strcmp(path, "logs/sensor_0001.bin") == 0, "sensor path");
    check(espnow_log_file_path(ESPNOW_LOG_FILE_ESPNOW, 12, path, sizeof(path)) != 0 && strcmp(path, "logs/espnow_0012.csv") == 0, "espnow path");
    check(espnow_log_file_path(ESPNOW_LOG_FILE_SENSOR, 12345, path, sizeof(path)) != 0 && strcmp(path, "logs/sensor_12345.bin") == 0,
          "a fifth digit is kept, not wrapped");
    check(espnow_log_file_path(ESPNOW_LOG_FILE_SENSOR, 1, path, 8) == 0, "a short buffer is refused");

    check(espnow_log_file_parse_name("sensor_0007.bin", &kind, &index) && kind == ESPNOW_LOG_FILE_SENSOR && index == 7, "sensor name");
    check(espnow_log_file_parse_name("ESPNOW_0123.CSV", &kind, &index) && kind == ESPNOW_LOG_FILE_ESPNOW && index == 123, "espnow name, any case");
    check(!espnow_log_file_parse_name("sensor_0007.csv", &kind, &index), "wrong extension");
    check(!espnow_log_file_parse_name("sensor_.bin", &kind, &index), "no digits");
    check(!espnow_log_file_parse_name("sensor_00x7.bin", &kind, &index), "digits with a letter");
    check(!espnow_log_file_parse_name("ERR_2026-10-07_01.LOG", &kind, &index), "the error log is not ours");
    check(!espnow_log_file_parse_name("error_0001.log", &kind, &index), "old error_NNNN.log is not counted");
    check(!espnow_log_file_parse_name("sensor_0001.bin.tmp", &kind, &index), "trailing text");
}

int main(void)
{
    printf("text\n");
    test_text();
    printf("csv\n");
    test_csv();
    printf("file names\n");
    test_names();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
