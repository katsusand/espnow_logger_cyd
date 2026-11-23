idf_component_register(SRCS "espnow_logger_settings_app.c" "espnow_logger_settings_view.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_shell cyd_display cyd_input cyd_ui espnow_logger)
