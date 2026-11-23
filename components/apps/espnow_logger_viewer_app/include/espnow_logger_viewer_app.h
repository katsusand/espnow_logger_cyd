#ifndef ESPNOW_LOGGER_VIEWER_APP_H
#define ESPNOW_LOGGER_VIEWER_APP_H

#include "app_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

const app_shell_app_t *espnow_logger_viewer_app_get_app(void);

/*
 * Registers the viewer with app_registry together with its settings screen
 * (espnow_logger_settings_app). Call from the composition, like
 * cyd_clock_app_register(). The composition also starts the service
 * (espnow_logger_start()); the viewer only shows what the service has.
 */
esp_err_t espnow_logger_viewer_app_register(void);

#ifdef __cplusplus
}
#endif

#endif
