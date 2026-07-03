idf_component_register(SRCS "hal_led.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board esp_driver_gpio esp_driver_ledc freertos log)
