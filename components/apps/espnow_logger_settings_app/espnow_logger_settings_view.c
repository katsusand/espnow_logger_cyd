#include "cyd_ui.h"
#include "espnow_logger_settings_view.h"

void espnow_logger_settings_view_build(cyd_display_screen_t *screen, const espnow_logger_settings_view_model_t *m)
{
    if (screen == NULL || m == NULL) {
        return;
    }
    cyd_ui_screen_clear(screen);
    cyd_ui_add_panel(screen, 0, 0, CYD_DISPLAY_GRID_COLS, CYD_DISPLAY_GRID_ROWS, CYD_UI_THEME_BG, 0, 0, 0);
    cyd_ui_add_settings_title(screen, "ESP-NOW の設定");
    cyd_ui_add_settings_back(screen, ESPNOW_LOGGER_SETTINGS_ACTION_BACK);

    (void)cyd_ui_add_switch_row(screen, "SDにログを記録", 5, m->log_requested, m->running,
                                ESPNOW_LOGGER_SETTINGS_ACTION_LOG);
    (void)cyd_ui_add_switch_row(screen, "起動時に自動で始める", 11, m->auto_start, true,
                                ESPNOW_LOGGER_SETTINGS_ACTION_AUTO_START);
    cyd_ui_add_label(screen, "カードが無いときは、入れるまで待ちます", 2, 17, 36, 3, CYD_DISPLAY_ALIGN_LEFT,
                     CYD_DISPLAY_FONT_BODY, CYD_UI_THEME_SUBTEXT);
}
