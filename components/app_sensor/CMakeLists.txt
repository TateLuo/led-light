idf_component_register(SRCS "app_sensor.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_state esp_timer hal_adc hal_led log freertos)
