idf_component_register(SRCS "app_ui.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_battery_test app_settings app_state esp_timer freertos
                             hal_display log lvgl__lvgl)
