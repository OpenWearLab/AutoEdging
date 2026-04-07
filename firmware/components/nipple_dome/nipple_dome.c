#include "nipple_dome.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "nipple_dome";

#define NIPPLE_DOME_GROUP_ID       0
#define NIPPLE_DOME_RESOLUTION_HZ  20000000UL

static int64_t dome_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool direction_is_driving(nipple_dome_direction_t direction)
{
    return direction == NIPPLE_DOME_DIRECTION_FORWARD || direction == NIPPLE_DOME_DIRECTION_REVERSE;
}

static nipple_dome_direction_t opposite_direction(nipple_dome_direction_t direction)
{
    switch (direction) {
    case NIPPLE_DOME_DIRECTION_FORWARD:
        return NIPPLE_DOME_DIRECTION_REVERSE;
    case NIPPLE_DOME_DIRECTION_REVERSE:
        return NIPPLE_DOME_DIRECTION_FORWARD;
    default:
        return NIPPLE_DOME_DIRECTION_FORWARD;
    }
}

static uint16_t clamp_permille(uint16_t permille)
{
    return permille > 1000 ? 1000 : permille;
}

static esp_err_t dome_apply_direction_locked(nipple_dome_t *dome,
                                             nipple_dome_direction_t direction,
                                             uint16_t duty_permille)
{
    if (!dome || !dome->gen_fwd || !dome->gen_rev || !dome->comparator) {
        return ESP_ERR_INVALID_STATE;
    }

    duty_permille = clamp_permille(duty_permille);
    if (duty_permille > 0 && duty_permille < 1000) {
        uint32_t compare_ticks = (dome->period_ticks * duty_permille + 500) / 1000;
        if (compare_ticks == 0) {
            compare_ticks = 1;
        }
        if (compare_ticks >= dome->period_ticks) {
            compare_ticks = dome->period_ticks - 1;
        }
        ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(dome->comparator, compare_ticks), TAG, "set compare failed");
    }

    switch (direction) {
    case NIPPLE_DOME_DIRECTION_STOP:
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(dome->gen_fwd, 0, true), TAG, "force stop fwd failed");
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(dome->gen_rev, 0, true), TAG, "force stop rev failed");
        dome->status.direction = NIPPLE_DOME_DIRECTION_STOP;
        dome->status.duty_permille = 0;
        return ESP_OK;
    case NIPPLE_DOME_DIRECTION_BRAKE:
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(dome->gen_fwd, 1, true), TAG, "force brake fwd failed");
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(dome->gen_rev, 1, true), TAG, "force brake rev failed");
        dome->status.direction = NIPPLE_DOME_DIRECTION_BRAKE;
        dome->status.duty_permille = 0;
        return ESP_OK;
    case NIPPLE_DOME_DIRECTION_FORWARD:
    case NIPPLE_DOME_DIRECTION_REVERSE: {
        mcpwm_gen_handle_t active = direction == NIPPLE_DOME_DIRECTION_FORWARD ? dome->gen_fwd : dome->gen_rev;
        mcpwm_gen_handle_t inactive = direction == NIPPLE_DOME_DIRECTION_FORWARD ? dome->gen_rev : dome->gen_fwd;
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(inactive, 0, true), TAG, "force inactive failed");
        if (duty_permille == 0) {
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(active, 0, true), TAG, "force active low failed");
        } else if (duty_permille >= 1000) {
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(active, 1, true), TAG, "force active high failed");
        } else {
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(active, -1, true), TAG, "release active failed");
        }
        dome->status.direction = direction;
        dome->status.duty_permille = duty_permille;
        return ESP_OK;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t dome_begin_transition_locked(nipple_dome_t *dome,
                                              nipple_dome_direction_t target_direction,
                                              uint16_t duty_permille,
                                              int64_t now_ms)
{
    if (!dome) {
        return ESP_ERR_INVALID_ARG;
    }

    duty_permille = clamp_permille(duty_permille);
    if (target_direction == NIPPLE_DOME_DIRECTION_BRAKE) {
        dome->brake_pending = false;
        dome->brake_until_ms = 0;
        dome->pending_direction = NIPPLE_DOME_DIRECTION_BRAKE;
        dome->pending_duty_permille = 0;
        return dome_apply_direction_locked(dome, NIPPLE_DOME_DIRECTION_BRAKE, 0);
    }

    nipple_dome_direction_t current = dome->status.direction;
    bool needs_brake = dome->config.brake_ms > 0 &&
                       direction_is_driving(current) &&
                       target_direction != current;

    if (!needs_brake) {
        dome->brake_pending = false;
        dome->brake_until_ms = 0;
        dome->pending_direction = target_direction;
        dome->pending_duty_permille = duty_permille;
        if (direction_is_driving(target_direction)) {
            dome->status.last_switch_ms = now_ms;
        }
        return dome_apply_direction_locked(dome, target_direction, duty_permille);
    }

    dome->pending_direction = target_direction;
    dome->pending_duty_permille = duty_permille;
    dome->brake_pending = true;
    dome->brake_until_ms = now_ms + (int64_t)dome->config.brake_ms;
    return dome_apply_direction_locked(dome, NIPPLE_DOME_DIRECTION_BRAKE, 0);
}

const char *nipple_dome_direction_to_string(nipple_dome_direction_t direction)
{
    switch (direction) {
    case NIPPLE_DOME_DIRECTION_STOP:
        return "stop";
    case NIPPLE_DOME_DIRECTION_FORWARD:
        return "forward";
    case NIPPLE_DOME_DIRECTION_REVERSE:
        return "reverse";
    case NIPPLE_DOME_DIRECTION_BRAKE:
        return "brake";
    default:
        return "unknown";
    }
}

const char *nipple_dome_mode_to_string(nipple_dome_mode_t mode)
{
    switch (mode) {
    case NIPPLE_DOME_MODE_DIRECT:
        return "direct";
    case NIPPLE_DOME_MODE_AUTO_OSCILLATE:
        return "auto_oscillate";
    default:
        return "unknown";
    }
}

esp_err_t nipple_dome_init(nipple_dome_t *dome, const nipple_dome_config_t *config)
{
    ESP_RETURN_ON_FALSE(dome && config, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config->gpio_fwd), ESP_ERR_INVALID_ARG, TAG, "invalid gpio_fwd");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(config->gpio_rev), ESP_ERR_INVALID_ARG, TAG, "invalid gpio_rev");
    ESP_RETURN_ON_FALSE(config->gpio_fwd != config->gpio_rev, ESP_ERR_INVALID_ARG, TAG, "gpio conflict");
    ESP_RETURN_ON_FALSE(config->pwm_hz > 0, ESP_ERR_INVALID_ARG, TAG, "invalid pwm_hz");

    memset(dome, 0, sizeof(*dome));
    dome->config = *config;
    dome->status.mode = NIPPLE_DOME_MODE_DIRECT;
    dome->status.direction = NIPPLE_DOME_DIRECTION_STOP;
    dome->status.duty_permille = 0;
    dome->period_ticks = NIPPLE_DOME_RESOLUTION_HZ / config->pwm_hz;
    ESP_RETURN_ON_FALSE(dome->period_ticks > 1, ESP_ERR_INVALID_ARG, TAG, "bad period");

    dome->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(dome->lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    mcpwm_timer_config_t timer_cfg = {
        .group_id = NIPPLE_DOME_GROUP_ID,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = NIPPLE_DOME_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = dome->period_ticks,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_cfg, &dome->timer), TAG, "new timer failed");

    mcpwm_operator_config_t operator_cfg = {
        .group_id = NIPPLE_DOME_GROUP_ID,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_operator(&operator_cfg, &dome->oper), TAG, "new operator failed");
    ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(dome->oper, dome->timer), TAG, "connect timer failed");

    mcpwm_comparator_config_t comparator_cfg = {
        .flags.update_cmp_on_tez = 1,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_comparator(dome->oper, &comparator_cfg, &dome->comparator), TAG, "new comparator failed");
    ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(dome->comparator, dome->period_ticks / 2), TAG, "init compare failed");

    mcpwm_generator_config_t gen_fwd_cfg = {
        .gen_gpio_num = config->gpio_fwd,
    };
    mcpwm_generator_config_t gen_rev_cfg = {
        .gen_gpio_num = config->gpio_rev,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(dome->oper, &gen_fwd_cfg, &dome->gen_fwd), TAG, "new gen fwd failed");
    ESP_RETURN_ON_ERROR(mcpwm_new_generator(dome->oper, &gen_rev_cfg, &dome->gen_rev), TAG, "new gen rev failed");

    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(
                            dome->gen_fwd,
                            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                         MCPWM_TIMER_EVENT_EMPTY,
                                                         MCPWM_GEN_ACTION_HIGH)),
                        TAG, "set fwd timer action failed");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(
                            dome->gen_fwd,
                            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                           dome->comparator,
                                                           MCPWM_GEN_ACTION_LOW)),
                        TAG, "set fwd compare action failed");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(
                            dome->gen_rev,
                            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                         MCPWM_TIMER_EVENT_EMPTY,
                                                         MCPWM_GEN_ACTION_HIGH)),
                        TAG, "set rev timer action failed");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(
                            dome->gen_rev,
                            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                           dome->comparator,
                                                           MCPWM_GEN_ACTION_LOW)),
                        TAG, "set rev compare action failed");

    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(dome->timer), TAG, "timer enable failed");
    ESP_RETURN_ON_ERROR(mcpwm_timer_start_stop(dome->timer, MCPWM_TIMER_START_NO_STOP), TAG, "timer start failed");
    ESP_RETURN_ON_ERROR(dome_apply_direction_locked(dome, NIPPLE_DOME_DIRECTION_STOP, 0), TAG, "init stop failed");

    ESP_LOGI(TAG, "init ok: gpio_fwd=%d gpio_rev=%d freq=%" PRIu32 "Hz brake=%" PRIu32 "ms",
             config->gpio_fwd, config->gpio_rev, config->pwm_hz, config->brake_ms);
    return ESP_OK;
}

esp_err_t nipple_dome_set_direct(nipple_dome_t *dome, nipple_dome_direction_t direction, uint16_t duty_permille)
{
    ESP_RETURN_ON_FALSE(dome != NULL, ESP_ERR_INVALID_ARG, TAG, "null dome");
    ESP_RETURN_ON_FALSE(direction >= NIPPLE_DOME_DIRECTION_STOP && direction <= NIPPLE_DOME_DIRECTION_BRAKE,
                        ESP_ERR_INVALID_ARG, TAG, "bad direction");
    if (xSemaphoreTake(dome->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    dome->status.mode = NIPPLE_DOME_MODE_DIRECT;
    dome->status.auto_enabled = false;
    dome->status.switch_period_ms = 0;

    esp_err_t err = dome_begin_transition_locked(dome,
                                                 direction,
                                                 direction_is_driving(direction) ? duty_permille : 0,
                                                 dome_now_ms());
    xSemaphoreGive(dome->lock);
    return err;
}

esp_err_t nipple_dome_set_auto(nipple_dome_t *dome, uint16_t duty_permille, uint32_t switch_period_ms, int64_t now_ms)
{
    ESP_RETURN_ON_FALSE(dome != NULL, ESP_ERR_INVALID_ARG, TAG, "null dome");
    if (xSemaphoreTake(dome->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    duty_permille = clamp_permille(duty_permille);
    dome->status.mode = NIPPLE_DOME_MODE_AUTO_OSCILLATE;
    dome->status.switch_period_ms = switch_period_ms;
    dome->status.auto_enabled = (duty_permille > 0 && switch_period_ms > 0);

    esp_err_t err = ESP_OK;
    if (!dome->status.auto_enabled) {
        err = dome_begin_transition_locked(dome, NIPPLE_DOME_DIRECTION_STOP, 0, now_ms);
        xSemaphoreGive(dome->lock);
        return err;
    }

    if (dome->brake_pending) {
        dome->pending_duty_permille = duty_permille;
        xSemaphoreGive(dome->lock);
        return ESP_OK;
    }

    if (!direction_is_driving(dome->status.direction)) {
        err = dome_begin_transition_locked(dome, NIPPLE_DOME_DIRECTION_FORWARD, duty_permille, now_ms);
    } else if ((now_ms - dome->status.last_switch_ms) >= (int64_t)switch_period_ms) {
        err = dome_begin_transition_locked(dome,
                                           opposite_direction(dome->status.direction),
                                           duty_permille,
                                           now_ms);
    } else {
        err = dome_apply_direction_locked(dome, dome->status.direction, duty_permille);
    }

    xSemaphoreGive(dome->lock);
    return err;
}

esp_err_t nipple_dome_update(nipple_dome_t *dome, int64_t now_ms)
{
    ESP_RETURN_ON_FALSE(dome != NULL, ESP_ERR_INVALID_ARG, TAG, "null dome");
    if (xSemaphoreTake(dome->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    if (dome->brake_pending && now_ms >= dome->brake_until_ms) {
        nipple_dome_direction_t next_direction = dome->pending_direction;
        uint16_t next_duty = dome->pending_duty_permille;
        dome->brake_pending = false;
        dome->brake_until_ms = 0;
        if (direction_is_driving(next_direction)) {
            dome->status.last_switch_ms = now_ms;
        }
        err = dome_apply_direction_locked(dome, next_direction, next_duty);
    } else if (dome->status.mode == NIPPLE_DOME_MODE_AUTO_OSCILLATE &&
               dome->status.auto_enabled &&
               direction_is_driving(dome->status.direction) &&
               dome->status.switch_period_ms > 0 &&
               (now_ms - dome->status.last_switch_ms) >= (int64_t)dome->status.switch_period_ms) {
        err = dome_begin_transition_locked(dome,
                                           opposite_direction(dome->status.direction),
                                           dome->status.duty_permille,
                                           now_ms);
    }

    xSemaphoreGive(dome->lock);
    return err;
}

esp_err_t nipple_dome_stop(nipple_dome_t *dome)
{
    return nipple_dome_set_direct(dome, NIPPLE_DOME_DIRECTION_STOP, 0);
}

void nipple_dome_get_status(nipple_dome_t *dome, nipple_dome_status_t *out)
{
    if (!dome || !out) {
        return;
    }
    if (xSemaphoreTake(dome->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *out = dome->status;
    xSemaphoreGive(dome->lock);
}
