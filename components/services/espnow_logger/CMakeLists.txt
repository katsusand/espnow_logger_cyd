# Case-insensitive, matching the top-level CMakeLists.txt: "OFF" must not
# select the real sources. The value is read from the environment because
# component CMakeLists run in a separate script-mode pass (see
# docs/build_feature_switches.md). Kconfig is not used to pick sources.
string(TOLOWER "$ENV{APP_ESPNOW}" APP_ESPNOW_ENV)
if(NOT "${APP_ESPNOW_ENV}" MATCHES "^(0|false|off|no)$")
    idf_component_register(SRCS "espnow_logger.c" "espnow_sensor_packet.c" "espnow_log_format.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES esp_event esp_netif esp_timer esp_wifi nvs_flash
                                      nvs_health nvs_schema error_log_store sd_card_status
                                      sd_card_storage sd_card_writer)
else()
    # The pure parts stay: they cost no driver and a viewer may format with them.
    idf_component_register(SRCS "espnow_logger_stub.c" "espnow_sensor_packet.c" "espnow_log_format.c"
                        INCLUDE_DIRS "include")
endif()
