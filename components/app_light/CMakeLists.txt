idf_component_register(SRCS "app_light.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_safety app_state freertos hal_led hal_power log)
