#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_timer.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NIPPLE_DOME_DIRECTION_STOP = 0,
    NIPPLE_DOME_DIRECTION_FORWARD,
    NIPPLE_DOME_DIRECTION_REVERSE,
    NIPPLE_DOME_DIRECTION_BRAKE,
} nipple_dome_direction_t;

typedef enum {
    NIPPLE_DOME_MODE_DIRECT = 0,
    NIPPLE_DOME_MODE_AUTO_OSCILLATE,
} nipple_dome_mode_t;

typedef struct {
    gpio_num_t gpio_fwd;
    gpio_num_t gpio_rev;
    uint32_t pwm_hz;
    uint32_t brake_ms;
} nipple_dome_config_t;

typedef struct {
    nipple_dome_mode_t mode;
    nipple_dome_direction_t direction;
    uint16_t duty_permille;
    bool auto_enabled;
    uint32_t switch_period_ms;
    int64_t last_switch_ms;
} nipple_dome_status_t;

typedef struct {
    nipple_dome_config_t config;
    nipple_dome_status_t status;
    SemaphoreHandle_t lock;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator;
    mcpwm_gen_handle_t gen_fwd;
    mcpwm_gen_handle_t gen_rev;
    uint32_t period_ticks;
    nipple_dome_direction_t pending_direction;
    uint16_t pending_duty_permille;
    int64_t brake_until_ms;
    bool brake_pending;
} nipple_dome_t;

esp_err_t nipple_dome_init(nipple_dome_t *dome, const nipple_dome_config_t *config);
esp_err_t nipple_dome_set_direct(nipple_dome_t *dome, nipple_dome_direction_t direction, uint16_t duty_permille);
esp_err_t nipple_dome_set_auto(nipple_dome_t *dome, uint16_t duty_permille, uint32_t switch_period_ms, int64_t now_ms);
esp_err_t nipple_dome_update(nipple_dome_t *dome, int64_t now_ms);
esp_err_t nipple_dome_stop(nipple_dome_t *dome);
void nipple_dome_get_status(nipple_dome_t *dome, nipple_dome_status_t *out);

const char *nipple_dome_direction_to_string(nipple_dome_direction_t direction);
const char *nipple_dome_mode_to_string(nipple_dome_mode_t mode);

#ifdef __cplusplus
}
#endif
