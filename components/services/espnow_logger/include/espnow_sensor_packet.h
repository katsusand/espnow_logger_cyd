#pragma once

/*
 * ESP-NOW sensor packet v2 and the sensor.bin record (v1): parse and encode.
 *
 * English contract: pure C, no ESP-IDF headers, so host tests link the real
 * source. The packed structs are the on-air / on-disk layout and assume a
 * little-endian CPU (ESP32 and the host test machines are). The byte layout is
 * specified in docs/sensor_log_format.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_SENSOR_MAGIC 0xA55A
#define ESPNOW_SENSOR_PACKET_VERSION 2
#define ESPNOW_SENSOR_RECORD_VERSION 1

#define ESPNOW_SENSOR_TYPE_IMU 1
#define ESPNOW_SENSOR_TYPE_GPS 2
#define ESPNOW_SENSOR_TYPE_UTC_TIME 3
#define ESPNOW_SENSOR_TYPE_IMU_BATCH 4
#define ESPNOW_SENSOR_TYPE_IMU_CONTROL 5
#define ESPNOW_SENSOR_TYPE_ATTITUDE 6
#define ESPNOW_SENSOR_TYPE_IMU_BATCH_NOSUMMARY 7

#define ESPNOW_SENSOR_IMU_CONTROL_START 1
#define ESPNOW_SENSOR_IMU_CONTROL_STOP 2

#define ESPNOW_SENSOR_IMU_BATCH_MAX_SAMPLES 10

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
} espnow_sensor_packet_header_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t command;
} espnow_sensor_imu_control_packet_t;

typedef struct __attribute__((packed)) {
    uint32_t sample_seq;
    int16_t ax_mg;
    int16_t ay_mg;
    int16_t az_mg;
    int16_t gx_mdps;
    int16_t gy_mdps;
    int16_t gz_mdps;
    int16_t temp_cdeg;
    uint16_t flags;
} espnow_sensor_imu_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t sample_seq;
    int32_t lat_e7;
    int32_t lon_e7;
    int32_t alt_mm;
    uint32_t speed_mmps;
    uint16_t course_cdeg;
    uint16_t hdop_centi;
    uint8_t sats;
    uint8_t fix_type;
    uint16_t flags;
    uint64_t gps_utc_ms;
} espnow_sensor_gps_payload_t;

typedef struct __attribute__((packed)) {
    int16_t accel_mg[3];
    int16_t gyro_centi_dps[3];
} espnow_sensor_imu_raw_sample_t;

typedef struct __attribute__((packed)) {
    int16_t accel_mg_avg[3];
    int16_t accel_mg_min[3];
    int16_t accel_mg_max[3];
    int16_t gyro_centi_dps_avg[3];
    int16_t gyro_centi_dps_min[3];
    int16_t gyro_centi_dps_max[3];
} espnow_sensor_imu_batch_summary_t;

typedef struct __attribute__((packed)) {
    uint32_t sequence_start;
    uint8_t sample_count;
    uint8_t sample_period_ms;
    uint16_t flags;
    int16_t temperature_centi_c;
    uint64_t gps_utc_ms;
    espnow_sensor_imu_batch_summary_t summary;
    espnow_sensor_imu_raw_sample_t raw[ESPNOW_SENSOR_IMU_BATCH_MAX_SAMPLES];
} espnow_sensor_imu_batch_payload_t;

/* Type 7: the same as type 4 without the summary block. */
typedef struct __attribute__((packed)) {
    uint32_t sequence_start;
    uint8_t sample_count;
    uint8_t sample_period_ms;
    uint16_t flags;
    int16_t temperature_centi_c;
    uint64_t gps_utc_ms;
    espnow_sensor_imu_raw_sample_t raw[ESPNOW_SENSOR_IMU_BATCH_MAX_SAMPLES];
} espnow_sensor_imu_batch_nosummary_payload_t;

/* Type 6: attitude in the vehicle frame; `sequence` is the paired batch's sequence_start. */
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    uint64_t gps_utc_ms;
    int16_t roll_centi_deg;
    int16_t pitch_centi_deg;
    int16_t yaw_rate_centi_dps;
    int16_t long_accel_mg;
    int16_t lat_accel_mg;
    uint16_t speed_cmps;
    uint16_t flags;
} espnow_sensor_attitude_payload_t;

typedef struct __attribute__((packed)) {
    uint8_t utc_year_le[2];
    uint8_t utc_month;
    uint8_t utc_day;
    uint8_t utc_hour;
    uint8_t utc_minute;
    uint8_t utc_second;
} espnow_sensor_utc_time_payload_t;

/* sensor.bin record header (v1). A record is header + payload + crc32. */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t header_len;
    uint16_t payload_len;
    uint32_t seq;
    uint64_t ts_ms;
} espnow_sensor_record_header_t;

_Static_assert(sizeof(espnow_sensor_packet_header_t) == 4, "packet header must be 4 bytes");
_Static_assert(sizeof(espnow_sensor_imu_control_packet_t) == 5, "imu control packet must be 5 bytes");
_Static_assert(sizeof(espnow_sensor_imu_payload_t) == 20, "imu payload must be 20 bytes");
_Static_assert(sizeof(espnow_sensor_gps_payload_t) == 36, "gps payload must be 36 bytes");
_Static_assert(sizeof(espnow_sensor_imu_raw_sample_t) == 12, "imu raw sample must be 12 bytes");
_Static_assert(sizeof(espnow_sensor_imu_batch_summary_t) == 36, "imu batch summary must be 36 bytes");
_Static_assert(sizeof(espnow_sensor_imu_batch_payload_t) == 174, "imu batch payload must be 174 bytes");
_Static_assert(sizeof(espnow_sensor_imu_batch_nosummary_payload_t) == 138, "imu batch (type 7) payload must be 138 bytes");
_Static_assert(sizeof(espnow_sensor_attitude_payload_t) == 26, "attitude payload must be 26 bytes");
_Static_assert(sizeof(espnow_sensor_utc_time_payload_t) == 7, "utc time payload must be 7 bytes");
_Static_assert(sizeof(espnow_sensor_record_header_t) == 20, "record header must be 20 bytes");

#define ESPNOW_SENSOR_RECORD_CRC_LEN 4U
/* Largest record: header + the biggest payload (IMU batch) + crc32. */
#define ESPNOW_SENSOR_RECORD_MAX_LEN \
    (sizeof(espnow_sensor_record_header_t) + sizeof(espnow_sensor_imu_batch_payload_t) + ESPNOW_SENSOR_RECORD_CRC_LEN)

typedef enum {
    /* Magic does not match: not ours. Goes to espnow.csv as any other frame. */
    ESPNOW_SENSOR_KIND_NOT_SENSOR = 0,
    ESPNOW_SENSOR_KIND_IMU,
    ESPNOW_SENSOR_KIND_GPS,
    ESPNOW_SENSOR_KIND_IMU_BATCH,
    /* Type 7: IMU batch without summary. Same data as IMU_BATCH minus the summary. */
    ESPNOW_SENSOR_KIND_IMU_BATCH_NOSUMMARY,
    /* Type 6: attitude (vehicle frame). */
    ESPNOW_SENSOR_KIND_ATTITUDE,
    /* Valid UTC time broadcast; utc_epoch_ms is set. Not a sensor record. */
    ESPNOW_SENSOR_KIND_UTC_TIME,
    /* Valid IMU control packet; control_command is set. Not a sensor record. */
    ESPNOW_SENSOR_KIND_IMU_CONTROL,
    /* Magic matches but version, type, length or contents are wrong. */
    ESPNOW_SENSOR_KIND_DECODE_ERROR,
} espnow_sensor_kind_t;

typedef struct {
    espnow_sensor_kind_t kind;
    union {
        espnow_sensor_imu_payload_t imu;
        espnow_sensor_gps_payload_t gps;
        espnow_sensor_imu_batch_payload_t imu_batch;
        espnow_sensor_imu_batch_nosummary_payload_t imu_batch_nosummary;
        espnow_sensor_attitude_payload_t attitude;
    } body;
    uint8_t control_command;
    /* Unix epoch ms that the packet carries (GPS / IMU batch / attitude gps_utc_ms, or the
       UTC time packet); 0 when it carries none. A receiver may set its clock. */
    uint64_t utc_epoch_ms;
    /* Set with DECODE_ERROR: a one-line reason for the error log. */
    char error[96];
} espnow_sensor_packet_t;

/*
 * Sorts one ESP-NOW payload. `out` is always filled in (kind NOT_SENSOR for
 * anything that is not ours). Never reads past `len`.
 */
void espnow_sensor_packet_parse(const uint8_t *payload, size_t len, espnow_sensor_packet_t *out);

/* Builds the 5 byte IMU stream start/stop request. */
void espnow_sensor_packet_build_imu_control(uint8_t command, espnow_sensor_imu_control_packet_t *out);

/* Same as the UTC time packet's payload decoded to Unix epoch ms; false if invalid. */
bool espnow_sensor_utc_payload_to_epoch_ms(const espnow_sensor_utc_time_payload_t *payload, uint64_t *epoch_ms);

/* CRC-32 (IEEE, as zlib / binascii.crc32). `crc` is 0 to start, or a previous result to continue. */
uint32_t espnow_sensor_crc32(uint32_t crc, const void *data, size_t len);

/*
 * Writes one sensor.bin record (header + payload + crc32) into `out`. `kind` must be
 * IMU, GPS, IMU_BATCH, IMU_BATCH_NOSUMMARY or ATTITUDE and the payload is taken from `packet->body`. Returns the
 * record length, or 0 when `kind` is not a sensor record or `out_size` is too small.
 */
size_t espnow_sensor_record_encode(const espnow_sensor_packet_t *packet,
                                   uint32_t seq,
                                   uint64_t ts_ms,
                                   uint8_t *out,
                                   size_t out_size);

#ifdef __cplusplus
}
#endif
