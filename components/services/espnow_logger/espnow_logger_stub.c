#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "espnow_logger.h"

#define TAG "espnow_logger"

/* Built when APP_ESPNOW=0: the same API, nothing behind it. */

esp_err_t espnow_logger_start(void)
{
    ESP_LOGW(TAG, "ESP-NOW logger is disabled by APP_ESPNOW=0");
    return ESP_OK;
}

bool espnow_logger_is_started(void)
{
    return false;
}

uint8_t espnow_logger_get_channel(void)
{
    return 0;
}

esp_err_t espnow_logger_set_channel(uint8_t channel)
{
    (void)channel;
    return ESP_ERR_NOT_SUPPORTED;
}

bool espnow_logger_has_configured_peer(void)
{
    return false;
}

esp_err_t espnow_logger_get_local_mac(uint8_t mac[6])
{
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac is null");
    memset(mac, 0, 6);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_get_configured_peer_mac(uint8_t mac[6])
{
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_INVALID_ARG, TAG, "mac is null");
    memset(mac, 0, 6);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t espnow_logger_set_configured_peer_mac(const uint8_t mac[6])
{
    (void)mac;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_send_broadcast(const void *payload, size_t len)
{
    (void)payload;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_send_to_configured_peer(const void *payload, size_t len)
{
    (void)payload;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_send_to_mac(const uint8_t dest_mac[6], const void *payload, size_t len)
{
    (void)dest_mac;
    (void)payload;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_send_imu_stream_start_request(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_send_imu_stream_stop_request(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_send_imu_stream_request_to_mac(const uint8_t dest_mac[6], bool start)
{
    (void)dest_mac;
    (void)start;
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t espnow_logger_get_event_sequence(void)
{
    return 0;
}

size_t espnow_logger_get_event_count(void)
{
    return 0;
}

size_t espnow_logger_copy_recent_events(espnow_logger_event_t *events, size_t max_events)
{
    (void)events;
    (void)max_events;
    return 0;
}

void espnow_logger_clear_events(void)
{
}

esp_err_t espnow_logger_start_logging(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_stop_logging(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espnow_logger_get_log_status(espnow_logger_log_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    memset(status, 0, sizeof(*status));
    return ESP_ERR_NOT_SUPPORTED;
}

bool espnow_logger_get_auto_start_log_enabled(void)
{
    return false;
}

esp_err_t espnow_logger_set_auto_start_log_enabled(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}
