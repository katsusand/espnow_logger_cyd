#ifndef ESPNOW_LOGGER_SETTINGS_VIEW_H
#define ESPNOW_LOGGER_SETTINGS_VIEW_H

/*
 * English contract: built from a model only, calls no service. One on/off row
 * (a switch, no "on"/"off" words) under the common settings title and back.
 * Starting and stopping the log is the viewer's control, not a setting: it is
 * not saved, and a boot starts from "auto start".
 */

#include <stdbool.h>
#include "cyd_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_LOGGER_SETTINGS_ACTION_BACK 0x5101
#define ESPNOW_LOGGER_SETTINGS_ACTION_AUTO_START 0x5103

typedef struct {
    bool auto_start;
} espnow_logger_settings_view_model_t;

void espnow_logger_settings_view_build(cyd_display_screen_t *screen, const espnow_logger_settings_view_model_t *m);

#ifdef __cplusplus
}
#endif

#endif
