#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

typedef struct {
    ledc_mode_t speed_mode;          // 建议 LEDC_LOW_SPEED_MODE（跨芯片更稳）
    ledc_timer_t timer;              // LEDC_TIMER_0..3
    uint32_t pwm_freq_hz;            // 例如 20000
    ledc_clk_cfg_t clk_cfg;          // LEDC_AUTO_CLK 通常够用
    uint32_t src_clk_hz;             // 用于自动选分辨率：一般填 0 表示内部默认；想精确可传 APB/REF 等
    ledc_timer_bit_t duty_resolution;// 如设为 0，内部用 ledc_find_suitable_duty_resolution() 选择
} pwm_ledc_timer_config_t;

typedef struct {
    int motor_id;                    // 0..3
    gpio_num_t gpio;
    ledc_channel_t channel;          // LEDC_CHANNEL_0..7
    uint32_t init_duty_permille;     // 初始占空比，千分比 0..1000
    bool output_invert;              // 是否反相输出
} pwm_ledc_channel_config_t;

typedef struct {
    pwm_ledc_timer_config_t timer_cfg;
    pwm_ledc_channel_config_t ch_cfg[4];
} pwm_ledc_config_t;

typedef struct {
    pwm_ledc_config_t cfg;
    uint32_t duty_max;               // = (1<<resolution)-1
    ledc_timer_bit_t resolution;
    bool inited;
} pwm_ledc_t;

esp_err_t pwm_ledc_init(pwm_ledc_t *out, const pwm_ledc_config_t *cfg);
esp_err_t pwm_ledc_set_permille(pwm_ledc_t *pwm, int motor_id, uint32_t permille); // 0..1000
esp_err_t pwm_ledc_stop(pwm_ledc_t *pwm, int motor_id, uint32_t idle_level);      // idle_level: 0/1
