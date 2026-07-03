idf_component_register(SRCS "app_input.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_battery_test app_light app_ota app_safety app_settings app_state app_ui freertos
                             hal_display hal_fan hal_input hal_power log)
