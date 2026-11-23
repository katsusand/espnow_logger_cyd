#ifndef ESPNOW_LOGGER_SETTINGS_VIEW_H
#define ESPNOW_LOGGER_SETTINGS_VIEW_H

/*
 * English contract: built from a model only, calls no service. Two on/off rows
 * (switches, no "on"/"off" words) under the common settings title and back.
 */

#include <stdbool.h>
#include "cyd_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_LOGGER_SETTINGS_ACTION_BACK 0x5101
#define ESPNOW_LOGGER_SETTINGS_ACTION_LOG 0x5102
#define ESPNOW_LOGGER_SETTINGS_ACTION_AUTO_START 0x5103

typedef struct {
    /* The service is running; without it the log cannot be asked for. */
    bool running;
    /* What the user asked for: the SD log is recording (or waiting for a card). */
    bool log_requested;
    bool auto_start;
} espnow_logger_settings_view_model_t;

void espnow_logger_settings_view_build(cyd_display_screen_t *screen, const espnow_logger_settings_view_model_t *m);

#ifdef __cplusplus
}
#endif

#endif
