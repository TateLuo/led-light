idf_component_register(SRCS "hal_fan.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board esp_driver_gpio esp_driver_ledc log)
