idf_component_register(SRCS "app_battery_test.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_light app_safety app_sensor app_state esp_timer
                             freertos hal_led log)
