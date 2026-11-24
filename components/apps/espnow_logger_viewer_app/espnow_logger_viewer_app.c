#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_log.h"
#include "app_registry.h"
#include "app_shell.h"
#include "cyd_display.h"
#include "cyd_input.h"
#include "cyd_system_apps.h"
#include "cyd_ui.h"
#include "espnow_logger.h"
#include "espnow_logger_settings_app.h"
#include "espnow_logger_viewer_app.h"
#include "espnow_logger_viewer_view.h"
#include "sd_card_status.h"
#include "sd_card_status_icon.h"
#include "sdkconfig.h"

#define TAG "espnow_viewer"
/* A light poll inside app_shell's step, not a worker: it only checks the touch and the event sequence. */
#define ESPNOW_VIEWER_POLL_MS 100
#define ESPNOW_VIEWER_UNIQUE_WINDOW_MS (3ULL * 60ULL * 1000ULL)

static const cyd_ui_screen_owner_t s_screen_owner = { .name = "espnow_viewer" };

typedef struct {
    bool pending;
    bool long_pressed;
    uint16_t action_id;
} viewer_touch_tracker_t;

static viewer_touch_tracker_t s_tracker;
static uint32_t s_last_sequence;
static uint32_t s_last_sd_revision;
static size_t s_selected_index;
static bool s_render_needed;
/* What the IMU toggle showed last. The sender going quiet raises no event, so the step compares each poll. */
static bool s_last_imu_present;
static bool s_last_imu_streaming;
static bool s_pending_list_tap;
static espnow_logger_event_t s_events[CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE];
static size_t s_event_count;

static bool viewer_mac_is_broadcast(const uint8_t mac[ESPNOW_LOGGER_MAC_ADDR_LEN])
{
    static const uint8_t k_broadcast[ESPNOW_LOGGER_MAC_ADDR_LEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return memcmp(mac, k_broadcast, sizeof(k_broadcast)) == 0;
}

/* RX senders other than broadcast within 3 minutes of the newest frame. */
static unsigned viewer_count_unique_senders(void)
{
    static uint8_t seen[CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE][ESPNOW_LOGGER_MAC_ADDR_LEN];
    unsigned unique = 0;

    if (s_event_count == 0) {
        return 0;
    }
    for (size_t i = 0; i < s_event_count; ++i) {
        const espnow_logger_event_t *e = &s_events[i];
        bool known = false;

        if (e->direction != ESPNOW_LOGGER_EVENT_DIR_RX || viewer_mac_is_broadcast(e->src_mac) ||
            s_events[0].timestamp_ms - e->timestamp_ms > ESPNOW_VIEWER_UNIQUE_WINDOW_MS) {
            continue;
        }
        for (unsigned j = 0; j < unique; ++j) {
            if (memcmp(seen[j], e->src_mac, ESPNOW_LOGGER_MAC_ADDR_LEN) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            memcpy(seen[unique++], e->src_mac, ESPNOW_LOGGER_MAC_ADDR_LEN);
        }
    }
    return unique;
}

/* What the user asked for (the dot or the square), whatever the card does. */
static bool viewer_log_requested(void)
{
    espnow_logger_log_status_t status;

    if (espnow_logger_get_log_status(&status) != ESP_OK) {
        return false;
    }
    return status.requested;
}

/* Built in a static: it is about 0.2KB and only the shell task touches it. */
static espnow_logger_viewer_view_model_t s_model;

static void viewer_fill_model(void)
{
    espnow_logger_viewer_view_model_t *m = &s_model;

    memset(m, 0, sizeof(*m));
    m->running = espnow_logger_is_started();
    {
        espnow_logger_time_source_t source;

        m->imu_device_present = espnow_logger_get_time_source(&source);
        m->imu_streaming = m->imu_device_present && source.imu_streaming;
    }
    m->unique_senders = viewer_count_unique_senders();
    m->event_total = (unsigned)espnow_logger_get_event_count();
    m->log_requested = viewer_log_requested();
    /* The icon of the card's problem; NULL while it is fine. Its pixels live for the whole run. */
    /* Only an OK card takes the request: a problem, or no SD support at all (DISABLED), greys it out. */
    m->log_control_enabled = m->running && sd_card_status_get_state() == SD_CARD_STATUS_OK;
    m->sd_problem_icon = sd_card_status_icon_for_state(sd_card_status_get_state());
    m->channel = espnow_logger_get_channel();
    m->has_local_mac = espnow_logger_get_local_mac(m->local_mac) == ESP_OK;
    m->has_peer_mac = espnow_logger_has_configured_peer() &&
                      espnow_logger_get_configured_peer_mac(m->peer_mac) == ESP_OK;

    m->event_count = s_event_count < ESPNOW_LOGGER_VIEWER_VIEW_EVENTS ? s_event_count
                                                                     : ESPNOW_LOGGER_VIEWER_VIEW_EVENTS;
    m->selected_index = s_selected_index;
    for (size_t i = 0; i < m->event_count; ++i) {
        const espnow_logger_event_t *e = &s_events[i];
        espnow_logger_viewer_view_event_t *v = &m->events[i];

        v->is_rx = e->direction == ESPNOW_LOGGER_EVENT_DIR_RX;
        v->status = (espnow_logger_viewer_status_t)e->status; /* same order, see the enums */
        v->channel = e->channel;
        v->is_broadcast = e->is_broadcast;
        v->data_len = e->data_len;
        v->rssi = e->rssi;
        memcpy(v->src_mac, e->src_mac, sizeof(v->src_mac));
        memcpy(v->dst_mac, e->dst_mac, sizeof(v->dst_mac));
    }
    if (s_selected_index < m->event_count) {
        const espnow_logger_event_t *sel = &s_events[s_selected_index];

        m->payload_len = sel->payload_len;
        m->payload_shown = sel->payload_len < ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES
                               ? sel->payload_len
                               : ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES;
        memcpy(m->payload, sel->payload, m->payload_shown);
    }
}

static esp_err_t viewer_render(void)
{
    viewer_fill_model();
    espnow_logger_viewer_view_build(cyd_ui_screen_borrow(&s_screen_owner), &s_model);
    return cyd_ui_submit(cyd_ui_screen_of(&s_screen_owner));
}

static void viewer_refresh_events(void)
{
    s_event_count = espnow_logger_copy_recent_events(s_events, CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE);
    if (s_selected_index >= s_event_count || s_selected_index >= ESPNOW_LOGGER_VIEWER_VIEW_EVENTS) {
        s_selected_index = 0;
    }
}

/*
 * cyd_ui / cyd_display have no shared press-release tracker; the other apps
 * each keep their own, so this one does too: a button fires on release, on the
 * same button it was pressed on, and not after a long press.
 */
static bool viewer_confirmed_action(const cyd_input_event_t *event, uint16_t *action_id)
{
    if (event->type != CYD_INPUT_EVENT_TOUCH) {
        return false;
    }
    switch (event->data.touch.action) {
    case CYD_INPUT_TOUCH_ACTION_PRESS:
        s_tracker.pending = cyd_display_screen_hit_test(cyd_ui_screen_of(&s_screen_owner), event->data.touch.x,
                                                        event->data.touch.y, &s_tracker.action_id);
        s_tracker.long_pressed = false;
        return false;
    case CYD_INPUT_TOUCH_ACTION_LONG_PRESS:
    case CYD_INPUT_TOUCH_ACTION_REPEAT:
        if (s_tracker.pending) {
            s_tracker.long_pressed = true;
        }
        return false;
    case CYD_INPUT_TOUCH_ACTION_RELEASE: {
        uint16_t released = 0;
        bool confirmed = s_tracker.pending && !s_tracker.long_pressed &&
                         cyd_display_screen_hit_test(cyd_ui_screen_of(&s_screen_owner), event->data.touch.x,
                                                     event->data.touch.y, &released) &&
                         released == s_tracker.action_id;
        /* A press that landed on no button is a candidate for the event list. */
        s_pending_list_tap = !s_tracker.pending && !s_tracker.long_pressed;
        s_tracker = (viewer_touch_tracker_t){ 0 };
        if (confirmed && action_id != NULL) {
            *action_id = released;
        }
        return confirmed;
    }
    default:
        return false;
    }
}

static esp_err_t viewer_handle_action(uint16_t action_id)
{
    const uint8_t channel = espnow_logger_get_channel();
    esp_err_t err;

    switch (action_id) {
    case ESPNOW_LOGGER_VIEWER_ACTION_SETTINGS:
        /* The settings app returns here through app_shell_return_to(from_app). */
        return app_shell_switch_to(system_settings_app_get_app());
    case ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_DOWN:
        if (channel > ESPNOW_LOGGER_CHANNEL_MIN) {
            ESP_RETURN_ON_ERROR(espnow_logger_set_channel(channel - 1), TAG, "channel down failed");
            s_render_needed = true;
        }
        return ESP_OK;
    case ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_UP:
        if (channel < ESPNOW_LOGGER_CHANNEL_MAX) {
            ESP_RETURN_ON_ERROR(espnow_logger_set_channel(channel + 1), TAG, "channel up failed");
            s_render_needed = true;
        }
        return ESP_OK;
    case ESPNOW_LOGGER_VIEWER_ACTION_LOG_TOGGLE: {
        /* A greyed button is not hit-tested; this covers a card that went bad between the draw and the tap. */
        if (sd_card_status_get_state() != SD_CARD_STATUS_OK) {
            return ESP_OK;
        }
        /* A request already made stays as it is when the card goes: it carries on when the card is back. */
        err = viewer_log_requested() ? espnow_logger_stop_logging() : espnow_logger_start_logging();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "log request failed: %s", esp_err_to_name(err));
        }
        s_render_needed = true;
        return ESP_OK;
    }
    case ESPNOW_LOGGER_VIEWER_ACTION_IMU_TOGGLE: {
        espnow_logger_time_source_t source;

        /* A greyed button is not hit-tested; this covers a sender that went quiet between the draw and the tap. */
        if (!espnow_logger_get_time_source(&source)) {
            s_render_needed = true;
            return ESP_OK;
        }
        err = espnow_logger_send_imu_stream_request_to_mac(source.mac, !source.imu_streaming);
        if (err != ESP_OK) {
            /* A failed send is a state of the radio, not a fault of this app. */
            ESP_LOGW(TAG, "imu request failed: %s", esp_err_to_name(err));
        }
        return ESP_OK;
    }
    default:
        return ESP_OK;
    }
}

static esp_err_t viewer_enter(void *ctx, const app_shell_app_t *from_app)
{
    (void)ctx;
    (void)from_app;

    s_tracker = (viewer_touch_tracker_t){ 0 };
    s_selected_index = 0;
    s_pending_list_tap = false;
    s_last_sequence = espnow_logger_get_event_sequence();
    s_last_sd_revision = sd_card_status_get_revision();
    viewer_refresh_events();
    {
        espnow_logger_time_source_t source;

        s_last_imu_present = espnow_logger_get_time_source(&source);
        s_last_imu_streaming = s_last_imu_present && source.imu_streaming;
    }
    s_render_needed = false;
    return viewer_render();
}

static esp_err_t viewer_step(void *ctx)
{
    cyd_input_event_t event = { 0 };
    uint16_t action_id = 0;
    (void)ctx;

    if (cyd_input_read_event(&event, pdMS_TO_TICKS(ESPNOW_VIEWER_POLL_MS)) == ESP_OK) {
        if (viewer_confirmed_action(&event, &action_id)) {
            ESP_RETURN_ON_ERROR(viewer_handle_action(action_id), TAG, "viewer action failed");
        } else if (s_pending_list_tap) {
            size_t index = 0;

            s_pending_list_tap = false;
            if (espnow_logger_viewer_view_event_at(event.data.touch.x, event.data.touch.y, s_event_count, &index) &&
                index < ESPNOW_LOGGER_VIEWER_VIEW_EVENTS && index != s_selected_index) {
                s_selected_index = index;
                s_render_needed = true;
            }
        }
    }

    const uint32_t sequence = espnow_logger_get_event_sequence();
    if (sequence != s_last_sequence) {
        s_last_sequence = sequence;
        viewer_refresh_events();
        s_render_needed = true;
    }
    /* The card icon follows the card; the log state and the counts follow the frames or the next tap. */
    const uint32_t sd_revision = sd_card_status_get_revision();
    if (sd_revision != s_last_sd_revision) {
        s_last_sd_revision = sd_revision;
        s_render_needed = true;
    }
    {
        espnow_logger_time_source_t source;
        const bool present = espnow_logger_get_time_source(&source);
        const bool streaming = present && source.imu_streaming;

        if (present != s_last_imu_present || streaming != s_last_imu_streaming) {
            s_last_imu_present = present;
            s_last_imu_streaming = streaming;
            s_render_needed = true;
        }
    }
    if (s_render_needed) {
        s_render_needed = false;
        return viewer_render();
    }
    return ESP_OK;
}

static const app_shell_app_t s_viewer_app = {
    .id = "espnow_logger_viewer",
    .ctx = NULL,
    .enter = viewer_enter,
    .step = viewer_step,
    .leave = NULL,
};

const app_shell_app_t *espnow_logger_viewer_app_get_app(void)
{
    return &s_viewer_app;
}

esp_err_t espnow_logger_viewer_app_register(void)
{
    /* Borrowed by app_registry, so it must outlive registration. */
    static app_registry_entry_t entry = {
        .id = "espnow_logger",
        .title = "ESP-NOW",
    };

    entry.app = espnow_logger_viewer_app_get_app();
    entry.settings_app = espnow_logger_settings_app_get_app();
    ESP_RETURN_ON_ERROR(app_registry_register(&entry), TAG, "register espnow viewer failed");
    return ESP_OK;
}
