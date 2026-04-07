idf_component_register(SRCS "game_engine.c"
                    INCLUDE_DIRS "include"
                    REQUIRES act_pwm_ledc ble_belt dglab_socket nvs_flash led_strip nipple_dome)
