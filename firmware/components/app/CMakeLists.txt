idf_component_register(SRCS "app.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bus_i2c dev_mcp_h11 dev_dac7571 act_pwm_ledc ble_belt game_engine control_api telemetry web_server nvs_flash esp_wifi esp_event esp_netif spiffs mdns led_strip)
