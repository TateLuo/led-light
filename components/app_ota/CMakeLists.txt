idf_component_register(SRCS "app_ota.c"
                    INCLUDE_DIRS "include"
                    REQUIRES app_battery_test app_light app_state app_update esp_event
                             esp_http_server esp_hw_support esp_netif
                             esp_system esp_wifi freertos log)
