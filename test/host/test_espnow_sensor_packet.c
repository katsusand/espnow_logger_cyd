/*
 * ESP-NOW sensor packet v2 and the sensor.bin record.
 *
 * What is protected here: the byte layout (sizes and offsets are the contract with
 * the M5Stick sender and with scripts/export_sensor_bin_csv.py), the length and
 * range checks that decide "record" versus "decode error", and the record's CRC.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "espnow_sensor_packet.h"

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

static size_t make_packet(uint8_t *out, uint8_t type, const void *body, size_t body_len)
{
    espnow_sensor_packet_header_t header = {.magic = ESPNOW_SENSOR_MAGIC, .version = 2, .type = type};
    memcpy(out, &header, sizeof(header));
    if (body_len > 0) {
        memcpy(out + sizeof(header), body, body_len);
    }
    return sizeof(header) + body_len;
}

static void test_layout(void)
{
    uint8_t bytes[8];
    espnow_sensor_packet_header_t header = {.magic = 0xA55A, .version = 2, .type = 4};

    memcpy(bytes, &header, sizeof(header));
    check(bytes[0] == 0x5A && bytes[1] == 0xA5 && bytes[2] == 2 && bytes[3] == 4, "packet header is 5A A5 02 type (little endian)");
    check(sizeof(espnow_sensor_packet_header_t) + sizeof(espnow_sensor_imu_payload_t) == 24, "IMU packet is 24 bytes");
    check(sizeof(espnow_sensor_packet_header_t) + sizeof(espnow_sensor_gps_payload_t) == 40, "GPS packet is 40 bytes");
    check(sizeof(espnow_sensor_packet_header_t) + sizeof(espnow_sensor_utc_time_payload_t) == 11, "UTC time packet is 11 bytes");
    check(sizeof(espnow_sensor_packet_header_t) + sizeof(espnow_sensor_imu_batch_payload_t) == 178, "IMU batch packet is 178 bytes");
    check(sizeof(espnow_sensor_imu_control_packet_t) == 5, "IMU control packet is 5 bytes");
    check(offsetof(espnow_sensor_gps_payload_t, gps_utc_ms) == 28, "GPS gps_utc_ms is at payload offset 28");
    check(offsetof(espnow_sensor_imu_batch_payload_t, gps_utc_ms) == 10, "batch gps_utc_ms is at payload offset 10");
    check(offsetof(espnow_sensor_imu_batch_payload_t, raw) == 54, "batch raw samples start at payload offset 54");
}

static void test_not_sensor(void)
{
    espnow_sensor_packet_t packet;
    uint8_t text[] = "espnow_logger:broadcast";
    uint8_t other_magic[] = {0x5A, 0xA6, 2, 1, 0, 0};

    espnow_sensor_packet_parse(text, sizeof(text) - 1, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_NOT_SENSOR, "ASCII payload is not a sensor packet");
    espnow_sensor_packet_parse(other_magic, sizeof(other_magic), &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_NOT_SENSOR, "another magic is not a sensor packet");
    espnow_sensor_packet_parse(text, 3, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_NOT_SENSOR, "shorter than the header is not a sensor packet");
    espnow_sensor_packet_parse(NULL, 10, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_NOT_SENSOR, "NULL payload is not a sensor packet");
}

static void test_imu_and_gps(void)
{
    uint8_t wire[64];
    espnow_sensor_packet_t packet;
    espnow_sensor_imu_payload_t imu = {.sample_seq = 7, .ax_mg = -1000, .az_mg = 1000, .temp_cdeg = 2512, .flags = 3};
    espnow_sensor_gps_payload_t gps = {.sample_seq = 9, .lat_e7 = 356812345, .lon_e7 = 1397671234, .alt_mm = 40000,
                                       .speed_mmps = 5000, .sats = 9, .fix_type = 2, .gps_utc_ms = 1709208000123ULL};
    size_t len = make_packet(wire, ESPNOW_SENSOR_TYPE_IMU, &imu, sizeof(imu));

    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_IMU, "IMU packet is parsed");
    check(packet.body.imu.sample_seq == 7 && packet.body.imu.ax_mg == -1000 && packet.body.imu.temp_cdeg == 2512,
          "IMU fields survive");
    check(packet.utc_epoch_ms == 0, "IMU carries no time");
    espnow_sensor_packet_parse(wire, len - 1, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR && strstr(packet.error, "imu packet length") != NULL,
          "IMU one byte short is a decode error that names the packet");
    espnow_sensor_packet_parse(wire, len + 1, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR, "IMU one byte long is a decode error");

    len = make_packet(wire, ESPNOW_SENSOR_TYPE_GPS, &gps, sizeof(gps));
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_GPS && packet.body.gps.lat_e7 == 356812345 && packet.body.gps.sats == 9,
          "GPS packet is parsed");
    check(packet.utc_epoch_ms == 1709208000123ULL, "GPS gps_utc_ms is the time it carries");
    gps.gps_utc_ms = 0;
    len = make_packet(wire, ESPNOW_SENSOR_TYPE_GPS, &gps, sizeof(gps));
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_GPS && packet.utc_epoch_ms == 0, "GPS with gps_utc_ms 0 carries no time");
}

static void test_batch(void)
{
    uint8_t wire[200];
    espnow_sensor_packet_t packet;
    espnow_sensor_imu_batch_payload_t batch;
    size_t len;

    memset(&batch, 0, sizeof(batch));
    batch.sequence_start = 100;
    batch.sample_count = 10;
    batch.sample_period_ms = 10;
    batch.gps_utc_ms = 1709208000000ULL;
    batch.raw[9].gyro_centi_dps[2] = -123;
    len = make_packet(wire, ESPNOW_SENSOR_TYPE_IMU_BATCH, &batch, sizeof(batch));
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_IMU_BATCH && packet.body.imu_batch.raw[9].gyro_centi_dps[2] == -123,
          "batch is parsed, last raw sample intact");
    check(packet.utc_epoch_ms == 1709208000000ULL, "batch gps_utc_ms is the time it carries");

    batch.sample_count = 0;
    len = make_packet(wire, ESPNOW_SENSOR_TYPE_IMU_BATCH, &batch, sizeof(batch));
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR && strstr(packet.error, "sample_count") != NULL, "batch of 0 samples is an error");
    batch.sample_count = 11;
    len = make_packet(wire, ESPNOW_SENSOR_TYPE_IMU_BATCH, &batch, sizeof(batch));
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR, "batch of 11 samples is an error (raw[] holds 10)");
    batch.sample_count = 10;
    len = make_packet(wire, ESPNOW_SENSOR_TYPE_IMU_BATCH, &batch, sizeof(batch));
    espnow_sensor_packet_parse(wire, len - 12, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR, "batch missing one raw sample is an error");
}

static void test_utc_time(void)
{
    uint8_t wire[32];
    espnow_sensor_packet_t packet;
    espnow_sensor_utc_time_payload_t utc = {.utc_year_le = {0xE8, 0x07}, .utc_month = 2, .utc_day = 29, .utc_hour = 12};
    size_t len = make_packet(wire, ESPNOW_SENSOR_TYPE_UTC_TIME, &utc, sizeof(utc));
    uint64_t epoch = 0;

    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_UTC_TIME && packet.utc_epoch_ms == 1709208000000ULL, "2024-02-29 12:00:00 UTC (a leap day)");

    utc.utc_year_le[0] = 0xE7; /* 2023 has no Feb 29 */
    len = make_packet(wire, ESPNOW_SENSOR_TYPE_UTC_TIME, &utc, sizeof(utc));
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR && packet.utc_epoch_ms == 0, "2023-02-29 is rejected, no time carried");

    utc = (espnow_sensor_utc_time_payload_t){.utc_year_le = {0xB2, 0x07}, .utc_month = 1, .utc_day = 1, .utc_second = 1};
    check(espnow_sensor_utc_payload_to_epoch_ms(&utc, &epoch) && epoch == 1000, "1970-01-01 00:00:01 is 1000 ms");
    utc.utc_year_le[0] = 0xB1; /* 1969 */
    check(!espnow_sensor_utc_payload_to_epoch_ms(&utc, &epoch), "before 1970 is rejected");
    utc = (espnow_sensor_utc_time_payload_t){.utc_year_le = {0xEA, 0x07}, .utc_month = 6, .utc_day = 26, .utc_hour = 12, .utc_minute = 34, .utc_second = 56};
    check(espnow_sensor_utc_payload_to_epoch_ms(&utc, &epoch) && epoch == 1782477296000ULL, "2026-06-26 12:34:56 UTC");
    utc.utc_second = 60;
    check(!espnow_sensor_utc_payload_to_epoch_ms(&utc, &epoch), "second 60 is rejected");
    utc.utc_second = 0;
    utc.utc_month = 13;
    check(!espnow_sensor_utc_payload_to_epoch_ms(&utc, &epoch), "month 13 is rejected");
}

static void test_control_and_errors(void)
{
    uint8_t wire[16];
    espnow_sensor_packet_t packet;
    espnow_sensor_imu_control_packet_t control;
    uint8_t bytes[5];

    espnow_sensor_packet_build_imu_control(ESPNOW_SENSOR_IMU_CONTROL_START, &control);
    memcpy(bytes, &control, sizeof(control));
    check(bytes[0] == 0x5A && bytes[1] == 0xA5 && bytes[2] == 2 && bytes[3] == 5 && bytes[4] == 1, "start request bytes: 5A A5 02 05 01");
    espnow_sensor_packet_parse(bytes, sizeof(bytes), &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_IMU_CONTROL && packet.control_command == 1, "control packet round trips");
    bytes[4] = 9;
    espnow_sensor_packet_parse(bytes, sizeof(bytes), &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR, "control command 9 is an error");
    espnow_sensor_packet_parse(bytes, 4, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR, "control packet without a command is an error");

    size_t len = make_packet(wire, 99, NULL, 0);
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR && strstr(packet.error, "type=99") != NULL, "unknown type is an error that names it");
    wire[2] = 1;
    espnow_sensor_packet_parse(wire, len, &packet);
    check(packet.kind == ESPNOW_SENSOR_KIND_DECODE_ERROR && strstr(packet.error, "version") != NULL, "version 1 is an error");
}

static void test_record(void)
{
    uint8_t wire[64];
    uint8_t record[ESPNOW_SENSOR_RECORD_MAX_LEN];
    espnow_sensor_packet_t packet;
    espnow_sensor_record_header_t header;
    espnow_sensor_imu_payload_t imu = {.sample_seq = 5, .ax_mg = 12};
    size_t len = make_packet(wire, ESPNOW_SENSOR_TYPE_IMU, &imu, sizeof(imu));
    uint32_t crc = 0;

    check(espnow_sensor_crc32(0, "123456789", 9) == 0xCBF43926U, "CRC-32 of 123456789 is CBF43926 (zlib)");
    check(espnow_sensor_crc32(espnow_sensor_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926U, "CRC-32 can be continued");

    espnow_sensor_packet_parse(wire, len, &packet);
    size_t total = espnow_sensor_record_encode(&packet, 42, 123456789ULL, record, sizeof(record));
    check(total == 20 + 20 + 4, "IMU record is header 20 + payload 20 + crc 4");
    memcpy(&header, record, sizeof(header));
    check(header.magic == 0xA55A && header.version == 1 && header.type == 1 && header.header_len == 20 && header.payload_len == 20,
          "record header fields");
    check(header.seq == 42 && header.ts_ms == 123456789ULL, "record seq and ts_ms");
    memcpy(&crc, record + total - 4, 4);
    check(crc == espnow_sensor_crc32(0, record, total - 4), "record crc32 covers header + payload");
    check(memcmp(record + 20, &imu, sizeof(imu)) == 0, "payload is stored as received");
    check(espnow_sensor_record_encode(&packet, 1, 1, record, total - 1) == 0, "a short buffer writes nothing");

    espnow_sensor_imu_batch_payload_t batch;
    memset(&batch, 0, sizeof(batch));
    batch.sample_count = 10;
    uint8_t big[200];
    len = make_packet(big, ESPNOW_SENSOR_TYPE_IMU_BATCH, &batch, sizeof(batch));
    espnow_sensor_packet_parse(big, len, &packet);
    total = espnow_sensor_record_encode(&packet, 1, 1, record, sizeof(record));
    check(total == ESPNOW_SENSOR_RECORD_MAX_LEN && total == 198, "batch record is the largest: 198 bytes");
    memcpy(&header, record, sizeof(header));
    check(header.type == 4 && header.payload_len == 174, "batch record type 4, payload 174");

    packet.kind = ESPNOW_SENSOR_KIND_UTC_TIME;
    check(espnow_sensor_record_encode(&packet, 1, 1, record, sizeof(record)) == 0, "a UTC time packet is not a record");
    packet.kind = ESPNOW_SENSOR_KIND_NOT_SENSOR;
    check(espnow_sensor_record_encode(&packet, 1, 1, record, sizeof(record)) == 0, "a foreign frame is not a record");
}

int main(void)
{
    printf("layout\n");
    test_layout();
    printf("not sensor\n");
    test_not_sensor();
    printf("IMU and GPS\n");
    test_imu_and_gps();
    printf("IMU batch\n");
    test_batch();
    printf("UTC time\n");
    test_utc_time();
    printf("control and errors\n");
    test_control_and_errors();
    printf("record\n");
    test_record();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
