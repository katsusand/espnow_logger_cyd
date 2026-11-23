#ifndef ESPNOW_LOGGER_SETTINGS_APP_H
#define ESPNOW_LOGGER_SETTINGS_APP_H

#include "app_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The settings screen of the ESP-NOW viewer: whether the SD log is recording
 * and whether it starts by itself at boot. Registered as the viewer's
 * `settings_app` (espnow_logger_viewer_app_register()), so it is a child of the
 * viewer and has no register function of its own.
 */
const app_shell_app_t *espnow_logger_settings_app_get_app(void);

#ifdef __cplusplus
}
#endif

#endif
