#include "espnow_sensor_packet.h"

#include <stdio.h>
#include <string.h>

static uint16_t read_le16(const uint8_t le[2])
{
    return (uint16_t)((uint16_t)le[0] | ((uint16_t)le[1] << 8));
}

static bool is_leap_year(uint16_t year)
{
    return ((year % 4U) == 0U && (year % 100U) != 0U) || ((year % 400U) == 0U);
}

static bool utc_payload_is_valid(const espnow_sensor_utc_time_payload_t *payload)
{
    static const uint8_t days_in_month[] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};

    if (payload == NULL) {
        return false;
    }
    uint16_t year = read_le16(payload->utc_year_le);
    if (year < 1970U || payload->utc_month < 1U || payload->utc_month > 12U) {
        return false;
    }
    if (payload->utc_hour > 23U || payload->utc_minute > 59U || payload->utc_second > 59U) {
        return false;
    }
    uint8_t max_day = days_in_month[payload->utc_month - 1U];
    if (payload->utc_month == 2U && is_leap_year(year)) {
        max_day = 29U;
    }
    return payload->utc_day >= 1U && payload->utc_day <= max_day;
}

bool espnow_sensor_utc_payload_to_epoch_ms(const espnow_sensor_utc_time_payload_t *payload, uint64_t *epoch_ms)
{
    if (!utc_payload_is_valid(payload) || epoch_ms == NULL) {
        return false;
    }

    /* Days from civil date (proleptic Gregorian), counted from 1970-01-01. */
    int64_t y = (int64_t)read_le16(payload->utc_year_le) - ((payload->utc_month <= 2U) ? 1 : 0);
    int64_t era = (y >= 0) ? (y / 400) : ((y - 399) / 400);
    int64_t year_of_era = y - era * 400;
    int64_t day_of_year =
        (153 * ((int64_t)payload->utc_month + ((payload->utc_month > 2U) ? -3 : 9)) + 2) / 5 + (int64_t)payload->utc_day - 1;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days = era * 146097 + day_of_era - 719468;
    if (days < 0) {
        return false;
    }
    int64_t seconds = days * 86400 + (int64_t)payload->utc_hour * 3600 + (int64_t)payload->utc_minute * 60 +
                      (int64_t)payload->utc_second;
    *epoch_ms = (uint64_t)seconds * 1000ULL;
    return true;
}

uint32_t espnow_sensor_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t value = ~crc;

    for (size_t i = 0; i < len; ++i) {
        value ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(value & 1U);
            value = (value >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~value;
}

static void set_error(espnow_sensor_packet_t *out, const char *fmt, unsigned a, unsigned b)
{
    out->kind = ESPNOW_SENSOR_KIND_DECODE_ERROR;
    (void)snprintf(out->error, sizeof(out->error), fmt, a, b);
}

void espnow_sensor_packet_parse(const uint8_t *payload, size_t len, espnow_sensor_packet_t *out)
{
    espnow_sensor_packet_header_t header;
    const size_t head = sizeof(header);

    memset(out, 0, sizeof(*out));
    out->kind = ESPNOW_SENSOR_KIND_NOT_SENSOR;
    if (payload == NULL || len < head) {
        return;
    }
    memcpy(&header, payload, head);
    if (header.magic != ESPNOW_SENSOR_MAGIC) {
        return;
    }
    if (header.version != ESPNOW_SENSOR_PACKET_VERSION) {
        set_error(out, "sensor packet version mismatch version=%u len=%u", header.version, (unsigned)len);
        return;
    }

    const uint8_t *body = payload + head;
    size_t expected = 0;

    switch (header.type) {
    case ESPNOW_SENSOR_TYPE_IMU:
        expected = head + sizeof(out->body.imu);
        if (len != expected) {
            set_error(out, "imu packet length mismatch len=%u expected=%u", (unsigned)len, (unsigned)expected);
            return;
        }
        memcpy(&out->body.imu, body, sizeof(out->body.imu));
        out->kind = ESPNOW_SENSOR_KIND_IMU;
        return;

    case ESPNOW_SENSOR_TYPE_IMU_BATCH:
        expected = head + sizeof(out->body.imu_batch);
        if (len != expected) {
            set_error(out, "imu batch packet length mismatch len=%u expected=%u", (unsigned)len, (unsigned)expected);
            return;
        }
        memcpy(&out->body.imu_batch, body, sizeof(out->body.imu_batch));
        if (out->body.imu_batch.sample_count == 0U ||
            out->body.imu_batch.sample_count > ESPNOW_SENSOR_IMU_BATCH_MAX_SAMPLES) {
            set_error(out, "imu batch sample_count invalid count=%u len=%u", out->body.imu_batch.sample_count, (unsigned)len);
            return;
        }
        out->kind = ESPNOW_SENSOR_KIND_IMU_BATCH;
        out->utc_epoch_ms = out->body.imu_batch.gps_utc_ms;
        return;

    case ESPNOW_SENSOR_TYPE_GPS:
        expected = head + sizeof(out->body.gps);
        if (len != expected) {
            set_error(out, "gps packet length mismatch len=%u expected=%u", (unsigned)len, (unsigned)expected);
            return;
        }
        memcpy(&out->body.gps, body, sizeof(out->body.gps));
        out->kind = ESPNOW_SENSOR_KIND_GPS;
        out->utc_epoch_ms = out->body.gps.gps_utc_ms;
        return;

    case ESPNOW_SENSOR_TYPE_UTC_TIME: {
        espnow_sensor_utc_time_payload_t utc;

        expected = head + sizeof(utc);
        if (len != expected) {
            set_error(out, "utc time packet length mismatch len=%u expected=%u", (unsigned)len, (unsigned)expected);
            return;
        }
        memcpy(&utc, body, sizeof(utc));
        if (!espnow_sensor_utc_payload_to_epoch_ms(&utc, &out->utc_epoch_ms)) {
            out->utc_epoch_ms = 0;
            out->kind = ESPNOW_SENSOR_KIND_DECODE_ERROR;
            (void)snprintf(out->error, sizeof(out->error), "utc time packet invalid date len=%u", (unsigned)len);
            return;
        }
        out->kind = ESPNOW_SENSOR_KIND_UTC_TIME;
        return;
    }

    case ESPNOW_SENSOR_TYPE_IMU_CONTROL: {
        espnow_sensor_imu_control_packet_t control;

        if (len != sizeof(control)) {
            set_error(out, "imu control packet length mismatch len=%u expected=%u", (unsigned)len, (unsigned)sizeof(control));
            return;
        }
        memcpy(&control, payload, sizeof(control));
        if (control.command != ESPNOW_SENSOR_IMU_CONTROL_START && control.command != ESPNOW_SENSOR_IMU_CONTROL_STOP) {
            set_error(out, "imu control command invalid command=%u len=%u", control.command, (unsigned)len);
            return;
        }
        out->kind = ESPNOW_SENSOR_KIND_IMU_CONTROL;
        out->control_command = control.command;
        return;
    }

    default:
        set_error(out, "unknown sensor packet type=%u len=%u", header.type, (unsigned)len);
        return;
    }
}

void espnow_sensor_packet_build_imu_control(uint8_t command, espnow_sensor_imu_control_packet_t *out)
{
    out->magic = ESPNOW_SENSOR_MAGIC;
    out->version = ESPNOW_SENSOR_PACKET_VERSION;
    out->type = ESPNOW_SENSOR_TYPE_IMU_CONTROL;
    out->command = command;
}

size_t espnow_sensor_record_encode(const espnow_sensor_packet_t *packet,
                                   uint32_t seq,
                                   uint64_t ts_ms,
                                   uint8_t *out,
                                   size_t out_size)
{
    const void *payload = NULL;
    size_t payload_len = 0;
    uint8_t type = 0;

    if (packet == NULL || out == NULL) {
        return 0;
    }
    switch (packet->kind) {
    case ESPNOW_SENSOR_KIND_IMU:
        type = ESPNOW_SENSOR_TYPE_IMU;
        payload = &packet->body.imu;
        payload_len = sizeof(packet->body.imu);
        break;
    case ESPNOW_SENSOR_KIND_GPS:
        type = ESPNOW_SENSOR_TYPE_GPS;
        payload = &packet->body.gps;
        payload_len = sizeof(packet->body.gps);
        break;
    case ESPNOW_SENSOR_KIND_IMU_BATCH:
        type = ESPNOW_SENSOR_TYPE_IMU_BATCH;
        payload = &packet->body.imu_batch;
        payload_len = sizeof(packet->body.imu_batch);
        break;
    default:
        return 0;
    }

    const size_t total = sizeof(espnow_sensor_record_header_t) + payload_len + ESPNOW_SENSOR_RECORD_CRC_LEN;
    if (out_size < total) {
        return 0;
    }

    const espnow_sensor_record_header_t header = {
        .magic = ESPNOW_SENSOR_MAGIC,
        .version = ESPNOW_SENSOR_RECORD_VERSION,
        .type = type,
        .header_len = (uint16_t)sizeof(espnow_sensor_record_header_t),
        .payload_len = (uint16_t)payload_len,
        .seq = seq,
        .ts_ms = ts_ms,
    };
    memcpy(out, &header, sizeof(header));
    memcpy(out + sizeof(header), payload, payload_len);
    uint32_t crc = espnow_sensor_crc32(0, out, sizeof(header) + payload_len);
    memcpy(out + sizeof(header) + payload_len, &crc, sizeof(crc));
    return total;
}
