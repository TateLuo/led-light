idf_component_register(SRCS "hal_display.c"
                    INCLUDE_DIRS "include"
                    REQUIRES board esp_driver_gpio esp_driver_ledc esp_driver_spi
                             esp_lcd esp_timer freertos log
                             lvgl__lvgl)
