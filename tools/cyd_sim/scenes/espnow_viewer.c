/*
 * The ESP-NOW viewer and its settings, built from hand-made models. The
 * service is device-only, so the scenes feed the view the frames a sniffer
 * would show.
 */
#include <string.h>
#include "espnow_logger_settings_view.h"
#include "espnow_logger_viewer_view.h"
#include "sim_catalog.h"

/* The device's own icon art (pure: no display, no card), so the scenes show the real pixels.
   sd_card_status_icon.c itself needs the firmware's Kconfig and log, so it is not built here. */
#include "../../../components/services/sd_card_status/sd_card_status_icon_art.c"

static const cyd_display_bitmap_t *problem_icon(sd_card_status_icon_kind_t kind)
{
    static uint16_t pixels[SD_CARD_STATUS_ICON_KIND_COUNT][SD_CARD_STATUS_ICON_SIZE_PX * SD_CARD_STATUS_ICON_SIZE_PX];
    static cyd_display_bitmap_t bitmaps[SD_CARD_STATUS_ICON_KIND_COUNT];

    if (bitmaps[kind].data == NULL) {
        (void)sd_card_status_icon_art_render(kind, pixels[kind]);
        bitmaps[kind] = (cyd_display_bitmap_t){ .data = pixels[kind], .width_px = SD_CARD_STATUS_ICON_SIZE_PX,
                                                .height_px = SD_CARD_STATUS_ICON_SIZE_PX };
    }
    return &bitmaps[kind];
}

static const uint8_t k_local[6] = { 0x24, 0x6F, 0x28, 0xA1, 0xB2, 0xC3 };
static const uint8_t k_peer[6] = { 0x7C, 0xDF, 0xA1, 0x00, 0x11, 0x22 };
static const uint8_t k_bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static espnow_logger_viewer_view_model_t base(void)
{
    espnow_logger_viewer_view_model_t m;

    memset(&m, 0, sizeof(m));
    m.running = true;
    m.unique_senders = 2;
    m.event_total = 128;
    m.log_requested = true;
    m.log_control_enabled = true;
    m.channel = 13;
    m.has_local_mac = true;
    memcpy(m.local_mac, k_local, 6);
    m.has_peer_mac = true;
    memcpy(m.peer_mac, k_peer, 6);
    m.event_count = 4;
    m.events[0] = (espnow_logger_viewer_view_event_t){ .is_rx = true, .status = ESPNOW_LOGGER_VIEWER_STATUS_RECV,
                                                       .channel = 13, .data_len = 38, .rssi = -61 };
    memcpy(m.events[0].src_mac, k_peer, 6);
    memcpy(m.events[0].dst_mac, k_local, 6);
    m.events[1] = (espnow_logger_viewer_view_event_t){ .status = ESPNOW_LOGGER_VIEWER_STATUS_DONE, .channel = 13,
                                                       .is_broadcast = true, .data_len = 23 };
    memcpy(m.events[1].src_mac, k_local, 6);
    memcpy(m.events[1].dst_mac, k_bcast, 6);
    m.events[2] = (espnow_logger_viewer_view_event_t){ .status = ESPNOW_LOGGER_VIEWER_STATUS_FAIL, .channel = 13,
                                                       .data_len = 21 };
    memcpy(m.events[2].src_mac, k_local, 6);
    memcpy(m.events[2].dst_mac, k_peer, 6);
    m.events[3] = (espnow_logger_viewer_view_event_t){ .is_rx = true, .status = ESPNOW_LOGGER_VIEWER_STATUS_RECV,
                                                       .channel = 13, .is_broadcast = true, .data_len = 250,
                                                       .rssi = -102 };
    memcpy(m.events[3].src_mac, k_bcast, 6);
    memcpy(m.events[3].dst_mac, k_bcast, 6);
    for (size_t i = 0; i < ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES; ++i) {
        m.payload[i] = (uint8_t)(0xA0 + i * 7);
    }
    m.payload_shown = ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES;
    m.payload_len = 38;
    return m;
}

static void build_events(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    const espnow_logger_viewer_view_model_t m = base();
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_selected_last(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    espnow_logger_viewer_view_model_t m = base();
    m.selected_index = 3;
    m.payload_len = 250;
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_short_payload(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    espnow_logger_viewer_view_model_t m = base();
    m.selected_index = 1;
    m.payload_shown = 9;
    m.payload_len = 9;
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_empty(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    espnow_logger_viewer_view_model_t m = base();
    m.event_count = 0;
    m.event_total = 0;
    m.unique_senders = 0;
    m.log_requested = false;
    m.has_peer_mac = false;
    m.channel = 1;
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_stopped(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    espnow_logger_viewer_view_model_t m = base();
    m.running = false;
    m.log_control_enabled = false;
    m.has_local_mac = false;
    m.event_count = 0;
    m.channel = 14;
    m.log_requested = false;
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_log_stopped(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    espnow_logger_viewer_view_model_t m = base();
    m.log_requested = false;
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_problem(cyd_display_screen_t *screen, sd_card_status_icon_kind_t kind, bool requested)
{
    espnow_logger_viewer_view_model_t m = base();
    m.log_requested = requested;
    m.log_control_enabled = false;
    m.sd_problem_icon = problem_icon(kind);
    espnow_logger_viewer_view_build(screen, &m);
}

static void build_no_card(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    build_problem(screen, SD_CARD_STATUS_ICON_NO_CARD, true);
}

static void build_no_filesystem(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    build_problem(screen, SD_CARD_STATUS_ICON_NO_FILESYSTEM, true);
}

static void build_full(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    build_problem(screen, SD_CARD_STATUS_ICON_FULL, true);
}

static void build_fault(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    build_problem(screen, SD_CARD_STATUS_ICON_FAULT, true);
}

static void build_no_card_idle(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    build_problem(screen, SD_CARD_STATUS_ICON_NO_CARD, false);
}

static void build_settings(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    const espnow_logger_settings_view_model_t m = { .auto_start = true };
    espnow_logger_settings_view_build(screen, &m);
}

static void build_settings_off(cyd_display_screen_t *screen, unsigned frame)
{
    (void)frame;
    const espnow_logger_settings_view_model_t m = { .auto_start = false };
    espnow_logger_settings_view_build(screen, &m);
}

static const cyd_sim_scene_t k_scenes[] = {
    { "espnow_events", "ESP-NOW 一覧、記録中 (●)", NULL, build_events },
    { "espnow_log_stopped", "ESP-NOW ログ停止中 (■)", NULL, build_log_stopped },
    { "espnow_sd_no_card", "ESP-NOW カードなし (斜線) + ●", NULL, build_no_card },
    { "espnow_sd_no_filesystem", "ESP-NOW 未フォーマット (?) + ●", NULL, build_no_filesystem },
    { "espnow_sd_full", "ESP-NOW 満杯 (メーター) + ●", NULL, build_full },
    { "espnow_sd_fault", "ESP-NOW 異常 (!) + ●", NULL, build_fault },
    { "espnow_sd_no_card_idle", "ESP-NOW カードなし (斜線) + ■", NULL, build_no_card_idle },
    { "espnow_selected_last", "ESP-NOW 長い payload", NULL, build_selected_last },
    { "espnow_short_payload", "ESP-NOW 短い payload", NULL, build_short_payload },
    { "espnow_empty", "ESP-NOW まだ届いていない", NULL, build_empty },
    { "espnow_stopped", "ESP-NOW 停止中", NULL, build_stopped },
    { "espnow_settings", "ESP-NOW の設定", NULL, build_settings },
    { "espnow_settings_stopped", "ESP-NOW の設定 (自動開始オフ)", NULL, build_settings_off },
};

CYD_SIM_REGISTER_SCENES(espnow_viewer, CYD_SIM_ORDER_MAIN_APP + 1, k_scenes);
