idf_component_register(SRCS "hal_input.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board esp_driver_gpio esp_timer freertos hal_power log)
