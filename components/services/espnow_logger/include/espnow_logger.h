#ifndef ESPNOW_LOGGER_H
#define ESPNOW_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP-NOW transport, a ring of recent frames, and the SD log of what arrives.
 *
 * English contract:
 *  - The service owns the Wi-Fi driver while it runs (STA mode, no connection),
 *    so it is exclusive with the STA Wi-Fi feature until coexistence is done.
 *    With APP_WIFI_STA enabled, espnow_logger_start() returns ESP_ERR_NOT_SUPPORTED.
 *  - It never mounts, formats or unmounts the card. That is sd_card_status. The
 *    log is written through sd_card_writer streams, opened while logging is
 *    wanted AND the card is OK, and closed when the card is not. Read the card
 *    itself from sd_card_status; read the log from espnow_logger_get_log_status().
 *  - Callbacks of the Wi-Fi task only copy bytes into streams; they never wait
 *    for the card.
 */

#define ESPNOW_LOGGER_MAC_ADDR_LEN 6
#define ESPNOW_LOGGER_MAX_DATA_LEN 250
#define ESPNOW_LOGGER_CHANNEL_MIN 1
#define ESPNOW_LOGGER_CHANNEL_MAX 14

typedef enum {
    ESPNOW_LOGGER_EVENT_DIR_TX = 0,
    ESPNOW_LOGGER_EVENT_DIR_RX,
} espnow_logger_event_direction_t;

typedef enum {
    ESPNOW_LOGGER_EVENT_STATUS_QUEUED = 0,
    ESPNOW_LOGGER_EVENT_STATUS_DONE,
    ESPNOW_LOGGER_EVENT_STATUS_FAIL,
    ESPNOW_LOGGER_EVENT_STATUS_RECV,
} espnow_logger_event_status_t;

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_ms;
    espnow_logger_event_direction_t direction;
    espnow_logger_event_status_t status;
    uint8_t channel;
    uint8_t src_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    uint8_t dst_mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    int16_t rssi;
    uint16_t data_len;
    uint16_t payload_len;
    bool is_broadcast;
    uint8_t payload[ESPNOW_LOGGER_MAX_DATA_LEN];
} espnow_logger_event_t;

typedef enum {
    /* Not asked to log. */
    ESPNOW_LOGGER_LOG_STOPPED = 0,
    /* Asked to log, but the card is not OK (see sd_card_status). Starts by itself when it is. */
    ESPNOW_LOGGER_LOG_WAITING_CARD,
    /* Streams are open; frames are being written. */
    ESPNOW_LOGGER_LOG_ACTIVE,
    /* Asked to log and the card is OK, but a stream failed or could not be opened. Retried. */
    ESPNOW_LOGGER_LOG_FAILED,
    /* Asked to stop; the streams are still being flushed. Safe to pull the card once STOPPED. */
    ESPNOW_LOGGER_LOG_STOPPING,
} espnow_logger_log_state_t;

typedef struct {
    uint64_t bytes_written;
    uint64_t bytes_dropped;
    uint32_t records_dropped;
    uint32_t files_opened;
    bool failed;
} espnow_logger_stream_stats_t;

typedef struct {
    espnow_logger_log_state_t state;
    /* What the user asked for (start_logging / stop_logging / auto start), whatever the card does. */
    bool requested;
    /* Why the last open or write failed; ESP_OK when nothing did. */
    esp_err_t last_error;
    /* sensor.bin records accepted since logging started (the record seq of the last one). */
    uint32_t sensor_records;
    /* Frames that arrived while no stream was open. */
    uint32_t frames_not_logged;
    espnow_logger_stream_stats_t sensor;
    espnow_logger_stream_stats_t espnow_csv;
} espnow_logger_log_status_t;

/* Wi-Fi + ESP-NOW up on the saved channel, saved peer. Needs nvs and the default event loop's owner (system boot). */
esp_err_t espnow_logger_start(void);
bool espnow_logger_is_started(void);

/* Current radio channel. set_channel() applies it and saves it for the next boot. */
uint8_t espnow_logger_get_channel(void);
esp_err_t espnow_logger_set_channel(uint8_t channel);

bool espnow_logger_has_configured_peer(void);
esp_err_t espnow_logger_get_local_mac(uint8_t mac[6]);
/* ESP_ERR_NOT_FOUND (mac zeroed) when none is set. */
esp_err_t espnow_logger_get_configured_peer_mac(uint8_t mac[6]);
/* Saves the peer used for unicast and IMU control. NULL clears it. */
esp_err_t espnow_logger_set_configured_peer_mac(const uint8_t mac[6]);

esp_err_t espnow_logger_send_broadcast(const void *payload, size_t len);
esp_err_t espnow_logger_send_to_configured_peer(const void *payload, size_t len);
esp_err_t espnow_logger_send_to_mac(const uint8_t dest_mac[6], const void *payload, size_t len);
esp_err_t espnow_logger_send_imu_stream_start_request(void);
esp_err_t espnow_logger_send_imu_stream_stop_request(void);
esp_err_t espnow_logger_send_imu_stream_request_to_mac(const uint8_t dest_mac[6], bool start);

/*
 * The device that sent a UTC time packet lately: the one an IMU start / stop request is for.
 * A time packet counts for ESPNOW_LOGGER_TIME_SOURCE_WINDOW_MS (the sender sends a few per minute, at most one per second), and
 * the device is "streaming" while IMU / IMU batch frames from the same MAC keep arriving
 * (within ESPNOW_LOGGER_IMU_STREAM_WINDOW_MS of now).
 */
#define ESPNOW_LOGGER_TIME_SOURCE_WINDOW_MS 30000U
#define ESPNOW_LOGGER_IMU_STREAM_WINDOW_MS 2000U

typedef struct {
    uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN];
    bool imu_streaming;
} espnow_logger_time_source_t;

/* False (and `out` untouched) when no device sent a time packet within the window. Safe from any task. */
bool espnow_logger_get_time_source(espnow_logger_time_source_t *out);

uint32_t espnow_logger_get_event_sequence(void);
size_t espnow_logger_get_event_count(void);
/* Newest first. */
size_t espnow_logger_copy_recent_events(espnow_logger_event_t *events, size_t max_events);
void espnow_logger_clear_events(void);

/*
 * Asks for the SD log. Returns at once: the streams are opened (or closed) by the
 * service's own task, when the card allows. ESP_ERR_INVALID_STATE before start.
 */
esp_err_t espnow_logger_start_logging(void);
/* Asks it to stop. Asynchronous: poll the status for STOPPED before the card is pulled. */
esp_err_t espnow_logger_stop_logging(void);
esp_err_t espnow_logger_get_log_status(espnow_logger_log_status_t *status);

/* Start logging by itself at boot. Saved. */
bool espnow_logger_get_auto_start_log_enabled(void);
esp_err_t espnow_logger_set_auto_start_log_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
