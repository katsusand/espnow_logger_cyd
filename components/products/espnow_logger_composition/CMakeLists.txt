idf_component_register(SRCS "espnow_logger_composition.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_shell
                             cyd_display
                             cyd_system_apps
                             error_log_store
                             espnow_logger
                             espnow_logger_viewer_app
                             nvs_health
                             sd_card_status
                             system_boot
                             time_sync)
