#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "dev_dac7571.h"
#include "act_pwm_ledc.h"
#include "led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_CONFIG_VERSION 5
#define GAME_PRESSURE_HISTORY_LEN 60

typedef enum {
    GAME_STATE_INITIAL_CALM = 0,
    GAME_STATE_MIDDLE,
    GAME_STATE_EDGING,
    GAME_STATE_DELAY,
    GAME_STATE_SUB_CALM,
} game_state_t;

typedef struct {
    float duration_min;
    float critical_pressure_kpa;
    float max_motor_intensity;
    float low_pressure_delay_s;
    float stimulation_ramp_rate_limit;
    float pressure_sensitivity;
    float stimulation_ramp_random_percent;
    float stimulation_ramp_random_interval_s;
    float intensity_gradual_increase;
    float shock_voltage;
    float mid_pressure_kpa;
    float mid_min_intensity;
    uint16_t pwm_max_permille[4];
    uint16_t pwm_min_permille[4];
} game_config_t;

typedef struct {
    bool running;
    bool paused;
    int64_t start_time_ms;
    int64_t end_time_ms;

    game_state_t state;
    int64_t state_timer_ms;
    float recorded_mid_intensity;

    float current_pressure;
    float max_pressure;
    float min_pressure;
    float average_pressure;

    float unrandom_intensity;
    float target_intensity;
    float current_intensity;
    float mid_intensity;
    float random_factor;
    int64_t last_update_ms;
    int64_t last_intensity_update_ms;
    int64_t last_random_update_ms;
    float prev_pressure;

    bool is_shocking;
    int64_t last_shock_time_ms;
    uint32_t shock_count;

    uint32_t edging_count;
    uint32_t total_denied_times;
    float total_stimulation_time_s;
} game_status_t;

typedef struct {
    dac7571_t *dac;
    pwm_ledc_t *pwm;
    SemaphoreHandle_t i2c_mutex;
    led_strip_handle_t led;
} game_engine_hw_t;

typedef struct {
    game_config_t config;
    game_status_t status;
    game_engine_hw_t hw;
    SemaphoreHandle_t lock;
    int64_t shock_end_time_ms;

    float pressure_history[GAME_PRESSURE_HISTORY_LEN];
    uint8_t pressure_hist_count;
    uint8_t pressure_hist_index;
    float pressure_hist_sum;

    uint16_t last_pwm_permille[4];
    uint8_t last_ble_swing;
    uint8_t last_ble_vibrate;
    uint16_t last_dac_code;
} game_engine_t;

void game_config_set_defaults(game_config_t *cfg);

esp_err_t game_config_validate(const game_config_t *cfg, char *err_msg, size_t err_len);

esp_err_t game_config_load(game_config_t *cfg);

esp_err_t game_config_save(const game_config_t *cfg);

esp_err_t game_engine_init(game_engine_t *g,
                           const game_engine_hw_t *hw,
                           const game_config_t *initial_cfg);

void game_engine_get_config(game_engine_t *g, game_config_t *out);

esp_err_t game_engine_set_config(game_engine_t *g, const game_config_t *cfg);

void game_engine_get_status(game_engine_t *g, game_status_t *out);

bool game_engine_is_running(game_engine_t *g);

void game_engine_start(game_engine_t *g, int64_t now_ms);

void game_engine_stop(game_engine_t *g);

void game_engine_set_paused(game_engine_t *g, bool paused);

void game_engine_trigger_shock(game_engine_t *g, bool force);

void game_engine_on_sample(game_engine_t *g, float pressure_kpa, int64_t now_ms);

#ifdef __cplusplus
}
#endif
