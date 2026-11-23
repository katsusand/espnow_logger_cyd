#ifndef ESPNOW_LOGGER_VIEWER_VIEW_H
#define ESPNOW_LOGGER_VIEWER_VIEW_H

/*
 * The ESP-NOW viewer's screen, built from a model only.
 *
 * English contract: espnow_logger_viewer_view_build() calls no service and
 * touches no global state, so cyd_sim and the host tests can build every state
 * by hand. It names no service header either: the model has its own small
 * event type, which the app fills from espnow_logger_event_t.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cyd_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_DOWN 0x5001
#define ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_UP 0x5002
#define ESPNOW_LOGGER_VIEWER_ACTION_SETTINGS 0x5003
#define ESPNOW_LOGGER_VIEWER_ACTION_IMU_START 0x5004
#define ESPNOW_LOGGER_VIEWER_ACTION_IMU_STOP 0x5005

#define ESPNOW_LOGGER_VIEWER_VIEW_EVENTS 4
#define ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES 20
#define ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MIN 1
#define ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MAX 14

/* Where the event list is, for the tap that selects a row (grid rows). */
#define ESPNOW_LOGGER_VIEWER_VIEW_LIST_FIRST_ROW 10
#define ESPNOW_LOGGER_VIEWER_VIEW_LIST_ROWS_PER_EVENT 2

typedef enum {
    ESPNOW_LOGGER_VIEWER_STATUS_QUEUED = 0,
    ESPNOW_LOGGER_VIEWER_STATUS_DONE,
    ESPNOW_LOGGER_VIEWER_STATUS_FAIL,
    ESPNOW_LOGGER_VIEWER_STATUS_RECV,
} espnow_logger_viewer_status_t;

/* The SD log, as the user sees it. Mirrors espnow_logger_log_state_t. */
typedef enum {
    ESPNOW_LOGGER_VIEWER_LOG_STOPPED = 0,
    ESPNOW_LOGGER_VIEWER_LOG_WAITING_CARD,
    ESPNOW_LOGGER_VIEWER_LOG_ACTIVE,
    ESPNOW_LOGGER_VIEWER_LOG_FAILED,
    ESPNOW_LOGGER_VIEWER_LOG_STOPPING,
} espnow_logger_viewer_log_t;

typedef struct {
    bool is_rx;
    espnow_logger_viewer_status_t status;
    uint8_t channel;
    bool is_broadcast;
    uint16_t data_len;
    int16_t rssi;
    uint8_t src_mac[6];
    uint8_t dst_mac[6];
} espnow_logger_viewer_view_event_t;

typedef struct {
    bool running;
    /* Distinct senders in the last 3 minutes, and every frame kept. */
    unsigned unique_senders;
    unsigned event_total;
    espnow_logger_viewer_log_t log;
    /* sd_card_status_icon_for_state(); NULL when the card needs no icon. */
    const cyd_display_bitmap_t *sd_icon;

    uint8_t channel;
    bool has_local_mac;
    uint8_t local_mac[6];
    bool has_peer_mac;
    uint8_t peer_mac[6];

    size_t event_count;
    size_t selected_index;
    espnow_logger_viewer_view_event_t events[ESPNOW_LOGGER_VIEWER_VIEW_EVENTS];
    /* Payload of the selected event: the first bytes, and its whole length. */
    uint8_t payload[ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES];
    size_t payload_shown;
    size_t payload_len;
} espnow_logger_viewer_view_model_t;

void espnow_logger_viewer_view_build(cyd_display_screen_t *screen, const espnow_logger_viewer_view_model_t *m);

/* The event row a tap at pixel (x, y) falls on, or false. */
bool espnow_logger_viewer_view_event_at(int16_t x, int16_t y, size_t event_count, size_t *index);

#ifdef __cplusplus
}
#endif

#endif
