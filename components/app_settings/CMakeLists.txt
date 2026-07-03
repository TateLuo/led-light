idf_component_register(SRCS "app_settings.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_state esp_timer freertos log nvs_flash)
