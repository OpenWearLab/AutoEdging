idf_component_register(SRCS "control_api.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash dev_dac7571 act_pwm_ledc ble_belt)
