idf_component_register(SRCS "test.c"
                    INCLUDE_DIRS "."
                    REQUIRES  bus_i2c dev_mcp_h11 dev_dac7571 act_pwm_ledc ble_belt nvs_flash)
