#include "freertos/FreeRTOS.h"
#include "esp_check.h"
#include "esp_log.h"
#include "app_shell.h"
#include "cyd_display.h"
#include "cyd_input.h"
#include "cyd_ui.h"
#include "espnow_logger.h"
#include "espnow_logger_settings_app.h"
#include "espnow_logger_settings_view.h"

#define TAG "espnow_settings"
#define ESPNOW_SETTINGS_POLL_MS 50

static const cyd_ui_screen_owner_t s_screen_owner = { .name = "espnow_settings" };
static const app_shell_app_t *s_return_app;

typedef struct {
    bool pending;
    bool long_pressed;
    uint16_t action_id;
} settings_touch_tracker_t;

static settings_touch_tracker_t s_tracker;

static esp_err_t settings_show(void)
{
    espnow_logger_log_status_t status = { 0 };
    espnow_logger_settings_view_model_t m = { 0 };

    m.running = espnow_logger_is_started();
    if (espnow_logger_get_log_status(&status) == ESP_OK) {
        m.log_requested = status.requested;
    }
    m.auto_start = espnow_logger_get_auto_start_log_enabled();
    espnow_logger_settings_view_build(cyd_ui_screen_borrow(&s_screen_owner), &m);
    return cyd_ui_submit(cyd_ui_screen_of(&s_screen_owner));
}

/* Release on the button that was pressed, not after a long press (as the other apps do). */
static bool settings_confirmed_action(const cyd_input_event_t *event, uint16_t *action_id)
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
        s_tracker = (settings_touch_tracker_t){ 0 };
        if (confirmed) {
            *action_id = released;
        }
        return confirmed;
    }
    default:
        return false;
    }
}

static esp_err_t settings_enter(void *ctx, const app_shell_app_t *from_app)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(from_app != NULL, ESP_ERR_INVALID_STATE, TAG, "return app not set");
    s_return_app = from_app;
    s_tracker = (settings_touch_tracker_t){ 0 };
    return settings_show();
}

static esp_err_t settings_step(void *ctx)
{
    cyd_input_event_t event = { 0 };
    uint16_t action_id = 0;
    (void)ctx;

    if (cyd_input_read_event(&event, pdMS_TO_TICKS(ESPNOW_SETTINGS_POLL_MS)) != ESP_OK ||
        !settings_confirmed_action(&event, &action_id)) {
        return ESP_OK;
    }
    switch (action_id) {
    case ESPNOW_LOGGER_SETTINGS_ACTION_BACK:
        return app_shell_return_to(s_return_app);
    case ESPNOW_LOGGER_SETTINGS_ACTION_LOG: {
        espnow_logger_log_status_t status = { 0 };
        esp_err_t err = ESP_OK;

        if (espnow_logger_get_log_status(&status) == ESP_OK) {
            err = status.requested ? espnow_logger_stop_logging() : espnow_logger_start_logging();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "log request failed: %s", esp_err_to_name(err));
        }
        return settings_show();
    }
    case ESPNOW_LOGGER_SETTINGS_ACTION_AUTO_START:
        ESP_RETURN_ON_ERROR(
            espnow_logger_set_auto_start_log_enabled(!espnow_logger_get_auto_start_log_enabled()), TAG,
            "saving auto start failed");
        return settings_show();
    default:
        return ESP_OK;
    }
}

static const app_shell_app_t s_settings_app = {
    .id = "espnow_logger_settings",
    .ctx = NULL,
    .enter = settings_enter,
    .step = settings_step,
    .leave = NULL,
};

const app_shell_app_t *espnow_logger_settings_app_get_app(void)
{
    return &s_settings_app;
}
