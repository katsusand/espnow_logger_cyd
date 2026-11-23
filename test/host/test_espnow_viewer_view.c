/*
 * The ESP-NOW viewer and its settings, measured with the real fonts: every
 * log state, every event kind at its widest, empty and stopped.
 */

#include <stdio.h>
#include <string.h>
#include "cyd_display.h"
#include "espnow_logger_settings_view.h"
#include "espnow_logger_viewer_view.h"
#include "sd_card_status_icon_art.h"
#include "ui_test_support.h"

static cyd_display_screen_t s_screen;
static const cyd_display_bitmap_t *problem_icon(sd_card_status_icon_kind_t kind);

static const cyd_display_widget_t *find_action(uint16_t action_id)
{
    for (size_t i = 0; i < s_screen.widget_count; ++i) {
        if (s_screen.widgets[i].type == CYD_DISPLAY_WIDGET_BUTTON && s_screen.widgets[i].action_id == action_id) {
            return &s_screen.widgets[i];
        }
    }
    return NULL;
}

static espnow_logger_viewer_view_model_t widest(void)
{
    espnow_logger_viewer_view_model_t m;

    memset(&m, 0, sizeof(m));
    m.running = true;
    m.log_control_enabled = true;
    m.unique_senders = 256;
    m.event_total = 256;
    m.channel = 14;
    m.has_local_mac = true;
    memset(m.local_mac, 0xEE, 6);
    m.has_peer_mac = true;
    memset(m.peer_mac, 0xEE, 6);
    m.event_count = ESPNOW_LOGGER_VIEWER_VIEW_EVENTS;
    for (size_t i = 0; i < m.event_count; ++i) {
        m.events[i] = (espnow_logger_viewer_view_event_t){ .is_rx = i % 2 == 0, .status = ESPNOW_LOGGER_VIEWER_STATUS_QUEUED,
                                                           .channel = 14, .is_broadcast = true, .data_len = 250,
                                                           .rssi = -100 };
        memset(m.events[i].src_mac, 0xEE, 6);
        memset(m.events[i].dst_mac, 0xEE, 6);
    }
    memset(m.payload, 0xEE, sizeof(m.payload));
    m.payload_shown = sizeof(m.payload);
    m.payload_len = 250;
    return m;
}

static void test_viewer_fits(void)
{
    espnow_logger_viewer_view_model_t m = widest();
    int bad = 0;

    for (int log = 0; log < 4; ++log) {
        for (size_t sel = 0; sel < ESPNOW_LOGGER_VIEWER_VIEW_EVENTS; ++sel) {
            for (int st = ESPNOW_LOGGER_VIEWER_STATUS_QUEUED; st <= ESPNOW_LOGGER_VIEWER_STATUS_RECV; ++st) {
                m.log_requested = (log & 1) != 0;
                m.sd_problem_icon = (log & 2) != 0 ? problem_icon(SD_CARD_STATUS_ICON_NO_CARD) : NULL;
                m.selected_index = sel;
                m.events[0].status = (espnow_logger_viewer_status_t)st;
                m.events[1].status = (espnow_logger_viewer_status_t)st;
                espnow_logger_viewer_view_build(&s_screen, &m);
                if (!ui_test_screen_fits(&s_screen)) {
                    ui_test_check_screen("viewer, widest events", &s_screen);
                    ++bad;
                }
            }
        }
    }
    ui_test_check(bad == 0, "every log control state, selection and status fits at its widest");

    m.running = false;
    m.has_peer_mac = false;
    m.has_local_mac = false;
    m.event_count = 0;
    m.channel = ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MIN;
    espnow_logger_viewer_view_build(&s_screen, &m);
    ui_test_check_screen("viewer, stopped and empty", &s_screen);
    ui_test_check(find_action(ESPNOW_LOGGER_VIEWER_ACTION_IMU_START) != NULL, "IMU start is drawn");
}

static void test_viewer_actions(void)
{
    espnow_logger_viewer_view_model_t m = widest();
    const cyd_display_widget_t *w;

    m.channel = ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MIN;
    espnow_logger_viewer_view_build(&s_screen, &m);
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_DOWN);
    ui_test_check(w != NULL && !w->enabled, "channel can not go below the lowest");
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_UP);
    ui_test_check(w != NULL && w->enabled, "channel can go up from the lowest");

    m.channel = ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MAX;
    espnow_logger_viewer_view_build(&s_screen, &m);
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_UP);
    ui_test_check(w != NULL && !w->enabled, "channel can not go above the highest");

    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_IMU_START);
    ui_test_check(w != NULL && w->enabled, "IMU is on while the service runs");
    m.has_peer_mac = false;
    espnow_logger_viewer_view_build(&s_screen, &m);
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_IMU_STOP);
    ui_test_check(w != NULL && w->enabled, "IMU still works without a peer (broadcast)");
    m.running = false;
    espnow_logger_viewer_view_build(&s_screen, &m);
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_IMU_STOP);
    ui_test_check(w != NULL && !w->enabled, "IMU is off while the service is stopped");
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_SETTINGS);
    ui_test_check(w != NULL && w->enabled, "settings is always there");
}

static int count_widgets(cyd_display_widget_type_t type, const cyd_display_bitmap_t *bitmap)
{
    int n = 0;

    for (size_t i = 0; i < s_screen.widget_count; ++i) {
        if (s_screen.widgets[i].type == type && (bitmap == NULL || s_screen.widgets[i].bitmap == bitmap)) {
            ++n;
        }
    }
    return n;
}

/* The real art of sd_card_status, rendered as sd_card_status_icon.c does. */
static const cyd_display_bitmap_t *problem_icon(sd_card_status_icon_kind_t kind)
{
    static uint16_t pixels[SD_CARD_STATUS_ICON_KIND_COUNT][SD_CARD_STATUS_ICON_SIZE_PX * SD_CARD_STATUS_ICON_SIZE_PX];
    static cyd_display_bitmap_t bitmaps[SD_CARD_STATUS_ICON_KIND_COUNT];

    if (bitmaps[kind].data == NULL) {
        ui_test_check(sd_card_status_icon_art_render(kind, pixels[kind]), "icon art renders");
        bitmaps[kind] = (cyd_display_bitmap_t){ .data = pixels[kind], .width_px = SD_CARD_STATUS_ICON_SIZE_PX,
                                                .height_px = SD_CARD_STATUS_ICON_SIZE_PX };
    }
    return &bitmaps[kind];
}

static void test_log_control(void)
{
    espnow_logger_viewer_view_model_t m = widest();
    const cyd_display_bitmap_t *ok = espnow_logger_viewer_view_sd_ok_icon();
    const cyd_display_widget_t *w;

    ui_test_check(ok->width_px == 16 && ok->height_px == 16, "the plain card is 16x16, two grid cells");

    /* Normal and each problem, with and without a request: the icon follows the card, the dot the request. */
    for (int kind = -1; kind < SD_CARD_STATUS_ICON_KIND_COUNT; ++kind) {
        for (int requested = 0; requested < 2; ++requested) {
            const cyd_display_bitmap_t *expect = kind < 0 ? ok : problem_icon((sd_card_status_icon_kind_t)kind);

            m.log_requested = requested != 0;
            m.sd_problem_icon = kind < 0 ? NULL : expect;
            /* What the app decides: only an OK card (no problem icon) takes a tap. */
            m.log_control_enabled = kind < 0;
            espnow_logger_viewer_view_build(&s_screen, &m);
            ui_test_check(count_widgets(CYD_DISPLAY_WIDGET_ICON, expect) == 1 &&
                              count_widgets(CYD_DISPLAY_WIDGET_ICON, NULL) == 1,
                          "one icon, the card's own");
            ui_test_check_screen("viewer, log control states", &s_screen);
            w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_LOG_TOGGLE);
            ui_test_check(w != NULL && w->enabled == (kind < 0),
                          "the log control takes a tap on an OK card and is greyed out on a problem");
            ui_test_check(w != NULL && w->span_cols * 8 >= 32 && w->span_rows * 8 >= 32,
                          "the log control is at least 32px");
        }
    }

    m.sd_problem_icon = NULL;
    m.log_control_enabled = false;
    espnow_logger_viewer_view_build(&s_screen, &m);
    w = find_action(ESPNOW_LOGGER_VIEWER_ACTION_LOG_TOGGLE);
    ui_test_check(w != NULL && !w->enabled, "the log control is off whenever the app says so (service stopped, or no SD support)");
}

static void test_event_hit(void)
{
    size_t index = 99;

    ui_test_check(!espnow_logger_viewer_view_event_at(10, 20, 4, &index), "above the list is not a row");
    ui_test_check(espnow_logger_viewer_view_event_at(10, ESPNOW_LOGGER_VIEWER_VIEW_LIST_FIRST_ROW * 8, 4, &index) &&
                      index == 0,
                  "the first row of the list is event 0");
    ui_test_check(espnow_logger_viewer_view_event_at(10, (ESPNOW_LOGGER_VIEWER_VIEW_LIST_FIRST_ROW + 5) * 8, 4,
                                                     &index) &&
                      index == 2,
                  "two grid rows per event");
    ui_test_check(!espnow_logger_viewer_view_event_at(10, (ESPNOW_LOGGER_VIEWER_VIEW_LIST_FIRST_ROW + 7) * 8, 3,
                                                      &index),
                  "a row with no event is not selectable");
    ui_test_check(!espnow_logger_viewer_view_event_at(10, 100, 4, NULL), "NULL index is refused");
}

static void test_settings(void)
{
    espnow_logger_settings_view_model_t m = { .auto_start = true };

    espnow_logger_settings_view_build(&s_screen, &m);
    ui_test_check_screen("viewer settings, auto start on", &s_screen);
    ui_test_check(find_action(ESPNOW_LOGGER_SETTINGS_ACTION_BACK) != NULL, "back is drawn");
    ui_test_check(find_action(ESPNOW_LOGGER_SETTINGS_ACTION_AUTO_START) != NULL, "auto start row is one button");
    m.auto_start = false;
    espnow_logger_settings_view_build(&s_screen, &m);
    ui_test_check_screen("viewer settings, auto start off", &s_screen);
}

int main(void)
{
    test_viewer_fits();
    test_viewer_actions();
    test_log_control();
    test_event_hit();
    test_settings();
    return ui_test_finish();
}
