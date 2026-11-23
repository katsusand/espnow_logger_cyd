#include "esp_check.h"
#include "espnow_logger_composition.h"

void app_main(void)
{
    ESP_ERROR_CHECK(espnow_logger_composition_start());
}
