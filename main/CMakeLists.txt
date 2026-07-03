idf_component_register(SRCS "app_main.c"
                    PRIV_REQUIRES app_battery_test app_input app_light app_ota app_safety app_sensor
                                   app_settings app_state app_ui board hal_adc
                                  freertos hal_display hal_fan hal_input hal_led
                                  hal_power log
                    INCLUDE_DIRS "")
