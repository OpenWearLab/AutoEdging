idf_component_register(SRCS "control_api.c"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash act_pwm_ledc ble_belt nipple_dome)
