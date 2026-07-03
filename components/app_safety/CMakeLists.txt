idf_component_register(SRCS "app_safety.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_settings app_state esp_timer freertos
                             hal_display hal_fan hal_led hal_power log)
