idf_component_register(SRCS "app.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bus_i2c dev_mcp_h11 act_pwm_ledc ble_belt dglab_socket nipple_dome game_engine control_api telemetry web_server wifi_service nvs_flash spiffs mdns led_strip)
