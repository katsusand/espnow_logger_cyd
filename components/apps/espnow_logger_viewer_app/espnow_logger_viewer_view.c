#include <stdio.h>
#include <string.h>
#include "cyd_ui.h"
#include "espnow_logger_viewer_view.h"

/*
 * 16px text only, on the 8px grid (40 x 30):
 *
 *   0-1    動作中  3分の送信元 2台  計128件
 *   2-5    チャンネル [−] 13 [＋]      [SD icon ● / ■] (red frame while recording)  (tap: start / stop the SD log)
 *   6-7    自分 AA:BB:CC:DD:EE:FF
 *   8-9    相手 AA:BB:CC:DD:EE:FF
 *   10-17  event list, one line per event (the selected one is framed)
 *   18-19  送信元 AA:BB:CC:DD:EE:FF   (selected event)
 *   20-21  宛先 FF:FF:FF:FF:FF:FF
 *   22-25  payload, two lines of hex
 *   26-29  [設定]                [IMU 開始 / 停止]  (right; grey while no device sends time packets)
 */
#define VIEW_BUTTON_ROW 26
#define VIEW_BUTTON_ROWS 4
#define VIEW_PAYLOAD_PER_LINE 10

static void view_mac(const uint8_t mac[6], char *out, size_t size)
{
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * The plain card of the log control, for a card that is fine. sd_card_status
 * has icons only for problems (sd_card_status_icon_for_state() is NULL for OK),
 * and those are drawn by the caller; this one is the view's own, the same 12x14
 * card shape in a 16x16 block on black. Symbols as in sd_card_status_icon_art.c:
 * . background, O outline, B body, G contacts.
 */
#define VIEW_ICON_PX 16
static const char *const s_card_plain[VIEW_ICON_PX] = {
    "................",
    "..OOOOOOOOO.....",
    "..OBBBBBBBBO....",
    "..OBBBBBBBBBO...",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBBBBBBBBBBO..",
    "..OBGBGBGBGBBO..",
    "..OBGBGBGBGBBO..",
    "..OOOOOOOOOOOO..",
    "................",
};

static uint16_t s_icon_pixels[VIEW_ICON_PX * VIEW_ICON_PX];
static cyd_display_bitmap_t s_icon_bitmap;
static bool s_icon_ready;

static uint16_t view_icon_color(char symbol)
{
    switch (symbol) {
    case 'O': return 0xc618; /* light grey */
    case 'B': return 0x2d7f; /* body: apart from every problem colour of sd_card_status */
    case 'G': return 0xfea0; /* gold contacts */
    default: return 0x0000;
    }
}

const cyd_display_bitmap_t *espnow_logger_viewer_view_sd_ok_icon(void)
{
    if (!s_icon_ready) {
        for (size_t y = 0; y < VIEW_ICON_PX; ++y) {
            for (size_t x = 0; x < VIEW_ICON_PX; ++x) {
                s_icon_pixels[y * VIEW_ICON_PX + x] = view_icon_color(s_card_plain[y][x]);
            }
        }
        s_icon_bitmap = (cyd_display_bitmap_t){ .data = s_icon_pixels, .width_px = VIEW_ICON_PX,
                                                .height_px = VIEW_ICON_PX };
        s_icon_ready = true;
    }
    return &s_icon_bitmap;
}

static const char *view_status_text(espnow_logger_viewer_status_t status)
{
    switch (status) {
    case ESPNOW_LOGGER_VIEWER_STATUS_QUEUED: return "待ち";
    case ESPNOW_LOGGER_VIEWER_STATUS_DONE: return "OK";
    case ESPNOW_LOGGER_VIEWER_STATUS_FAIL: return "NG";
    default: return "RX";
    }
}

static void view_label(cyd_display_screen_t *screen, const char *text, uint8_t col, uint8_t row, uint8_t span_cols,
                       cyd_display_font_t font, uint16_t color)
{
    cyd_ui_add_label(screen, text, col, row, span_cols, 2, CYD_DISPLAY_ALIGN_LEFT, font, color);
}

static void view_button(cyd_display_screen_t *screen, const char *text, uint8_t col, uint8_t row, uint8_t span_cols,
                        uint8_t span_rows, uint16_t action_id, bool enabled, bool primary)
{
    if (primary) {
        cyd_ui_add_styled_button(screen, text, col, row, span_cols, span_rows, CYD_DISPLAY_FONT_BODY_BOLD,
                                 CYD_UI_THEME_ON_PRIMARY, CYD_UI_THEME_PRIMARY, CYD_UI_THEME_PRIMARY, 0,
                                 action_id, enabled);
    } else {
        cyd_ui_add_styled_button(screen, text, col, row, span_cols, span_rows, CYD_DISPLAY_FONT_BODY_BOLD,
                                 CYD_UI_THEME_PRIMARY_SOFT, CYD_UI_THEME_SURFACE, CYD_UI_THEME_PRIMARY,
                                 CYD_UI_THEME_BORDER_PX, action_id, enabled);
    }
}

static void view_payload_line(const espnow_logger_viewer_view_model_t *m, size_t start, char *out, size_t size)
{
    size_t offset = 0;

    out[0] = '\0';
    for (size_t i = start; i < m->payload_shown && i < start + VIEW_PAYLOAD_PER_LINE && offset + 4 < size; ++i) {
        offset += (size_t)snprintf(out + offset, size - offset, i == start ? "%02X" : " %02X", m->payload[i]);
    }
    /* More bytes than the screen has room for: say so on the last line. */
    if (start > 0 && m->payload_len > m->payload_shown && offset + 4 < size) {
        snprintf(out + offset, size - offset, " …");
    }
}

void espnow_logger_viewer_view_build(cyd_display_screen_t *screen, const espnow_logger_viewer_view_model_t *m)
{
    char line[CYD_DISPLAY_TEXT_MAX_LEN + 1];
    char mac[18];

    if (screen == NULL || m == NULL) {
        return;
    }

    cyd_ui_screen_clear(screen);
    cyd_ui_add_panel(screen, 0, 0, CYD_DISPLAY_GRID_COLS, CYD_DISPLAY_GRID_ROWS, CYD_UI_THEME_BG, 0, 0, 0);

    view_label(screen, m->running ? "動作中" : "停止中", 1, 0, 8, CYD_DISPLAY_FONT_BODY_BOLD,
               m->running ? CYD_UI_THEME_SUCCESS_SOFT : CYD_UI_THEME_DANGER_SOFT);
    snprintf(line, sizeof(line), "3分の送信元 %u台  計%u件", m->unique_senders, m->event_total);
    view_label(screen, line, 8, 0, 30, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_TEXT);

    cyd_ui_add_label(screen, "チャンネル", 1, 2, 10, 4, CYD_DISPLAY_ALIGN_LEFT, CYD_DISPLAY_FONT_BODY_BOLD,
                     CYD_UI_THEME_TEXT);
    view_button(screen, "−", 11, 2, 5, 4, ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_DOWN,
                m->channel > ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MIN, false);
    snprintf(line, sizeof(line), "%u", (unsigned)m->channel);
    cyd_ui_add_label(screen, line, 16, 2, 5, 4, CYD_DISPLAY_ALIGN_CENTER, CYD_DISPLAY_FONT_BODY_BOLD,
                     CYD_UI_THEME_INFO);
    view_button(screen, "＋", 21, 2, 5, 4, ESPNOW_LOGGER_VIEWER_ACTION_CHANNEL_UP,
                m->channel < ESPNOW_LOGGER_VIEWER_VIEW_CHANNEL_MAX, false);
    /* The SD log control: the card, and a red dot (asked to record) or a square (not). One button, 12 x 4 cells. */
    {
        const bool on = m->log_control_enabled;
        const bool asked = m->log_requested;

        /* A disabled button is drawn in the disabled colours whatever it is given (as in a greyed switch row). */
        cyd_ui_add_styled_button(screen, "", 27, 2, 12, 4, CYD_DISPLAY_FONT_BODY_BOLD, CYD_UI_THEME_TEXT,
                                 on && asked ? CYD_UI_THEME_DANGER_TINT : CYD_UI_THEME_SURFACE,
                                 on ? (asked ? CYD_UI_THEME_DANGER_SOFT : CYD_UI_THEME_TEXT) : CYD_UI_THEME_DISABLED,
                                 CYD_UI_THEME_BORDER_PX, ESPNOW_LOGGER_VIEWER_ACTION_LOG_TOGGLE, on);
        /* The card icon is never greyed: it says what is wrong. */
        cyd_ui_add_icon(screen,
                        m->sd_problem_icon != NULL ? m->sd_problem_icon : espnow_logger_viewer_view_sd_ok_icon(),
                        28, 3, 2, 2);
        /* Both marks always: the record dot and the stop square. Only the frame tells the state. */
        cyd_ui_add_panel(screen, 31, 3, 2, 2, on ? CYD_UI_THEME_DANGER : CYD_UI_THEME_DISABLED,
                         on ? CYD_UI_THEME_DANGER : CYD_UI_THEME_DISABLED, 0, 8);
        cyd_ui_add_label(screen, "/", 33, 2, 2, 4, CYD_DISPLAY_ALIGN_CENTER, CYD_DISPLAY_FONT_BODY_BOLD,
                         on ? CYD_UI_THEME_TEXT : CYD_UI_THEME_DISABLED);
        cyd_ui_add_panel(screen, 35, 3, 2, 2, on ? CYD_UI_THEME_TEXT : CYD_UI_THEME_DISABLED,
                         on ? CYD_UI_THEME_TEXT : CYD_UI_THEME_DISABLED, 0, 0);
    }

    if (m->has_local_mac) {
        view_mac(m->local_mac, mac, sizeof(mac));
    } else {
        snprintf(mac, sizeof(mac), "--:--:--:--:--:--");
    }
    snprintf(line, sizeof(line), "自分 %s", mac);
    view_label(screen, line, 1, 6, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);
    if (m->has_peer_mac) {
        view_mac(m->peer_mac, mac, sizeof(mac));
    } else {
        snprintf(mac, sizeof(mac), "未設定");
    }
    snprintf(line, sizeof(line), "相手 %s", mac);
    view_label(screen, line, 1, 8, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);

    for (size_t i = 0; i < ESPNOW_LOGGER_VIEWER_VIEW_EVENTS; ++i) {
        const uint8_t row = (uint8_t)(ESPNOW_LOGGER_VIEWER_VIEW_LIST_FIRST_ROW +
                                      i * ESPNOW_LOGGER_VIEWER_VIEW_LIST_ROWS_PER_EVENT);
        if (i >= m->event_count) {
            if (i == 0) {
                cyd_ui_add_label(screen, "まだ届いていません", 1, row, 38, 2, CYD_DISPLAY_ALIGN_LEFT,
                                 CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);
            }
            continue;
        }
        const espnow_logger_viewer_view_event_t *e = &m->events[i];
        const bool selected = i == m->selected_index;
        char rssi[12] = "";
        char dir[12];
        char text[96]; /* roomy for the compiler; the label keeps what fits */

        if (selected) {
            cyd_ui_add_panel(screen, 0, row, CYD_DISPLAY_GRID_COLS, 2, CYD_UI_THEME_SURFACE, CYD_UI_THEME_PRIMARY, 1,
                             4);
        }
        /* The sender is the end that tells frames apart, so only its last three bytes are kept here;
           the whole address is under the list for the selected one. */
        if (e->is_rx) {
            snprintf(dir, sizeof(dir), "RX");
            snprintf(rssi, sizeof(rssi), " %d", (int)e->rssi);
        } else {
            snprintf(dir, sizeof(dir), "TX %s", view_status_text(e->status));
        }
        snprintf(text, sizeof(text), "%s %u %s %uB%s %02X:%02X:%02X", dir, (unsigned)e->channel,
                 e->is_broadcast ? "BC" : "UC", (unsigned)e->data_len, rssi, e->src_mac[3], e->src_mac[4],
                 e->src_mac[5]);
        view_label(screen, text, 1, row, 38, CYD_DISPLAY_FONT_BODY,
                   selected ? CYD_UI_THEME_INFO
                            : (e->status == ESPNOW_LOGGER_VIEWER_STATUS_FAIL ? CYD_UI_THEME_DANGER_SOFT
                                                                             : CYD_UI_THEME_TEXT));
    }

    if (m->event_count > 0 && m->selected_index < m->event_count) {
        const espnow_logger_viewer_view_event_t *e = &m->events[m->selected_index];
        char hex1[ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES * 3 + 8];
        char hex2[ESPNOW_LOGGER_VIEWER_VIEW_PAYLOAD_BYTES * 3 + 8];

        view_mac(e->src_mac, mac, sizeof(mac));
        snprintf(line, sizeof(line), "送信元 %s", mac);
        view_label(screen, line, 1, 18, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);
        view_mac(e->dst_mac, mac, sizeof(mac));
        snprintf(line, sizeof(line), "宛先 %s", mac);
        view_label(screen, line, 1, 20, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);
        if (m->payload_len == 0) {
            view_label(screen, "データなし", 1, 22, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);
        } else {
            view_payload_line(m, 0, hex1, sizeof(hex1));
            view_payload_line(m, VIEW_PAYLOAD_PER_LINE, hex2, sizeof(hex2));
            view_label(screen, hex1, 1, 22, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_TEXT);
            view_label(screen, hex2, 1, 24, 38, CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_TEXT);
        }
    }

    /* The request goes to the device that sends time packets, so it takes a tap only while there is one. */
    const bool imu = m->running && m->imu_device_present;
    const bool stop = imu && m->imu_streaming;
    view_button(screen, "設定", 1, VIEW_BUTTON_ROW, 12, VIEW_BUTTON_ROWS, ESPNOW_LOGGER_VIEWER_ACTION_SETTINGS, true,
                false);
    view_button(screen, stop ? "IMU 停止" : "IMU 開始", 27, VIEW_BUTTON_ROW, 12, VIEW_BUTTON_ROWS,
                ESPNOW_LOGGER_VIEWER_ACTION_IMU_TOGGLE, imu, !stop);
}

bool espnow_logger_viewer_view_event_at(int16_t x, int16_t y, size_t event_count, size_t *index)
{
    const int first = ESPNOW_LOGGER_VIEWER_VIEW_LIST_FIRST_ROW * CYD_DISPLAY_GRID_CELL_PX;
    const int per = ESPNOW_LOGGER_VIEWER_VIEW_LIST_ROWS_PER_EVENT * CYD_DISPLAY_GRID_CELL_PX;

    (void)x;
    if (index == NULL || y < first) {
        return false;
    }
    const size_t i = (size_t)(y - first) / (size_t)per;
    if (i >= event_count) {
        return false;
    }
    *index = i;
    return true;
}
