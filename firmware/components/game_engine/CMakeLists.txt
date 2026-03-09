idf_component_register(SRCS "game_engine.c"
                    INCLUDE_DIRS "include"
                    REQUIRES dev_dac7571 act_pwm_ledc ble_belt nvs_flash led_strip)
