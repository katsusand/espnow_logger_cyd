idf_component_register(SRCS "espnow_logger_viewer_app.c" "espnow_logger_viewer_view.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_registry app_shell cyd_display cyd_input cyd_system_apps cyd_ui
                             espnow_logger espnow_logger_settings_app sd_card_status)
