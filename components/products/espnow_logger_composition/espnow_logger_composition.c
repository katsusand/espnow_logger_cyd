#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_health.h"
#include "app_shell.h"
#include "cyd_display.h"
#include "cyd_system_apps.h"
#include "error_log_store.h"
#include "espnow_logger.h"
#include "espnow_logger_composition.h"
#include "espnow_logger_viewer_app.h"
#include "sd_card_status.h"
#include "settings_pin.h"
#include "system_boot.h"

#define TAG "espnow_logger_composition"

/* 2024-01-01T00:00:00Z. A clock earlier than this has never been set. */
#define ESPNOW_LOGGER_MIN_VALID_EPOCH_SEC 1704067200LL

#ifndef APP_BUILD_UNIX_TIME
#define APP_BUILD_UNIX_TIME ESPNOW_LOGGER_MIN_VALID_EPOCH_SEC
#endif

/*
 * Optional services must not brick the boot.
 *
 * English contract: the viewer is usable without the SD card, the scheduler and
 * the radio (it then shows what it has). Propagating those failures would reach
 * app_main's ESP_ERROR_CHECK and put a deployed device into a reboot loop, so
 * they are recorded and skipped. Only failures that leave the device unable to
 * render anything (system_boot) stay fatal.
 */
static void espnow_logger_composition_start_optional(const char *what, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }
    (void)error_log_store_append_esp_err(TAG, what, err);
    ESP_LOGW(TAG, "%s: %s; continuing in degraded mode", what, esp_err_to_name(err));
}

/*
 * Gives the clock a floor so log timestamps are not 1970 before the first UTC
 * packet arrives. Later, the service sets the clock from received UTC packets.
 */
static void espnow_logger_composition_seed_time_from_build(void)
{
    if (time(NULL) >= (time_t)ESPNOW_LOGGER_MIN_VALID_EPOCH_SEC) {
        return;
    }

    struct timeval build_time = { .tv_sec = (time_t)APP_BUILD_UNIX_TIME, .tv_usec = 0 };
    if (settimeofday(&build_time, NULL) != 0) {
        ESP_LOGW(TAG, "failed to seed system time from build timestamp");
        return;
    }
    ESP_LOGI(TAG, "seeded system time from build timestamp: %lld", (long long)build_time.tv_sec);
}

static void espnow_logger_composition_preflight_nvs_health(void)
{
    (void)cyd_display_get_brightness();
    (void)app_shell_get_idle_return_timeout_seconds();
}

esp_err_t espnow_logger_composition_start(void)
{
    system_boot_result_t boot_result = { 0 };
    const app_shell_app_t *initial_app = espnow_logger_viewer_app_get_app();
    esp_err_t err = system_boot_start(&boot_result);

    if (err != ESP_OK) {
        (void)error_log_store_append_esp_err(TAG, "system boot failed", err);
        ESP_RETURN_ON_ERROR(err, TAG, "system boot failed");
    }

    /* The card mounts on its own task; a missing card is a state, not a failure. */
    espnow_logger_composition_start_optional("sd card status start failed", sd_card_status_start());

    /*
     * No Wi-Fi time sync in this product, so the error log must not wait for a
     * clock answer. app_scheduler and time_tick are not started: only the
     * clock's alarms and face use them.
     */
    error_log_store_notify_time_decided(ERROR_LOG_TIME_UNAVAILABLE);
    espnow_logger_composition_seed_time_from_build();

    /*
     * The viewer is a logger: it must not drop back to a home screen, and the
     * logger has no other home to return to. 0 disables idle return.
     */
    espnow_logger_composition_start_optional("disable idle return failed",
                                             app_shell_set_idle_return_timeout_seconds(0));

    /*
     * Degraded mode: if the radio cannot start (a Wi-Fi STA build returns
     * ESP_ERR_NOT_SUPPORTED, or the driver fails), the viewer still runs and
     * shows an empty list, and settings stay reachable.
     */
    espnow_logger_composition_start_optional("espnow logger start failed", espnow_logger_start());

    espnow_logger_composition_start_optional("settings PIN init failed", settings_pin_init());
    espnow_logger_composition_preflight_nvs_health();
    if (nvs_health_requires_initialize()) {
        ESP_LOGW(TAG, "invalid NVS data detected; forcing Initialize NVS flow");
        system_settings_open_clear_nvs_confirm();
        initial_app = system_settings_app_get_app();
    }

    /* Holding the touch panel through boot opens settings (touch calibration is there). */
    if (boot_result.setup_shortcut_requested) {
        ESP_LOGI(TAG, "boot touch shortcut held: opening settings");
        initial_app = system_settings_app_get_app();
    }

    /* Registration lives here, not in an app's enter(): see cyd_clock_composition. */
    espnow_logger_composition_start_optional("register viewer app failed", espnow_logger_viewer_app_register());
    return app_shell_start(initial_app);
}
