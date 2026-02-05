#include "game_engine.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define SHOCK_ONCE_MS 1000
#include "nvs.h"
#include "nvs_flash.h"

#include "ble_belt.h"

#define GAME_NVS_NAMESPACE "game"
#define GAME_NVS_KEY_CFG "cfg"

#define DAC_VREF_VOLTAGE 3.3f
#define DAC_MAX_CODE 4095u
#define DAC_MAX_SAFE_VOLTAGE 2.1f

static const char *TAG = "game_engine";

typedef struct {
    uint32_t version;
    game_config_t cfg;
} game_config_blob_t;

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float rand_unit(void)
{
    uint32_t r = esp_random();
    float f = (r & 0xFFFFFF) / 16777215.0f;
    return (f - 0.5f) * 2.0f;
}

static uint16_t dac_code_from_voltage(float v)
{
    v = clampf(v, 0.0f, DAC_MAX_SAFE_VOLTAGE);
    float code = (v / DAC_VREF_VOLTAGE) * (float)DAC_MAX_CODE;
    if (code < 0.0f) code = 0.0f;
    if (code > (float)DAC_MAX_CODE) code = (float)DAC_MAX_CODE;
    return (uint16_t)(code + 0.5f);
}

void game_config_set_defaults(game_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->duration_min = 20.0f;
    cfg->critical_pressure_kpa = 23.0f;
    cfg->max_motor_intensity = 50.0f;
    cfg->low_pressure_delay_s = 15.0f;
    cfg->stimulation_ramp_rate_limit = 2.0f;
    cfg->pressure_sensitivity = 15.0f;
    cfg->stimulation_ramp_random_percent = 0.0f;
    cfg->intensity_gradual_increase = 2.0f;
    cfg->shock_voltage = 1.2f;
    cfg->mid_pressure_kpa = 19.0f;
    cfg->mid_min_intensity = 5.0f;
    for (int i = 0; i < 4; i++) {
        cfg->pwm_max_permille[i] = 500;
        cfg->pwm_min_permille[i] = 50;
    }
}

static esp_err_t validate_range_bool(bool ok, const char *msg, char *err_msg, size_t err_len)
{
    if (ok) {
        return ESP_OK;
    }
    if (err_msg && err_len > 0) {
        snprintf(err_msg, err_len, "%s", msg);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t game_config_validate(const game_config_t *cfg, char *err_msg, size_t err_len)
{
    if (!cfg) {
        return validate_range_bool(false, "cfg null", err_msg, err_len);
    }
    if (cfg->duration_min < 1.0f || cfg->duration_min > 120.0f) {
        return validate_range_bool(false, "duration out of range", err_msg, err_len);
    }
    if (cfg->critical_pressure_kpa < 0.0f || cfg->critical_pressure_kpa > 60.0f) {
        return validate_range_bool(false, "criticalPressure out of range", err_msg, err_len);
    }
    if (cfg->mid_pressure_kpa < 0.0f || cfg->mid_pressure_kpa > 60.0f) {
        return validate_range_bool(false, "midPressure out of range", err_msg, err_len);
    }
    if (cfg->max_motor_intensity < 1.0f || cfg->max_motor_intensity > 255.0f) {
        return validate_range_bool(false, "maxMotorIntensity out of range", err_msg, err_len);
    }
    if (cfg->low_pressure_delay_s < 0.0f || cfg->low_pressure_delay_s > 120.0f) {
        return validate_range_bool(false, "lowPressureDelay out of range", err_msg, err_len);
    }
    if (cfg->stimulation_ramp_rate_limit < 0.0f || cfg->stimulation_ramp_rate_limit > 10.0f) {
        return validate_range_bool(false, "stimulationRampRateLimit out of range", err_msg, err_len);
    }
    if (cfg->pressure_sensitivity < 0.0f || cfg->pressure_sensitivity > 30.0f) {
        return validate_range_bool(false, "pressureSensitivity out of range", err_msg, err_len);
    }
    if (cfg->stimulation_ramp_random_percent < 0.0f || cfg->stimulation_ramp_random_percent > 100.0f) {
        return validate_range_bool(false, "stimulationRampRandomPercent out of range", err_msg, err_len);
    }
    if (cfg->intensity_gradual_increase < 0.0f || cfg->intensity_gradual_increase > 50.0f) {
        return validate_range_bool(false, "intensityGradualIncrease out of range", err_msg, err_len);
    }
    if (cfg->shock_voltage < 0.0f || cfg->shock_voltage > DAC_MAX_SAFE_VOLTAGE) {
        return validate_range_bool(false, "shockVoltage out of range", err_msg, err_len);
    }
    if (cfg->mid_min_intensity < 0.0f || cfg->mid_min_intensity > cfg->max_motor_intensity) {
        return validate_range_bool(false, "midMinIntensity out of range", err_msg, err_len);
    }
    for (int i = 0; i < 4; i++) {
        if (cfg->pwm_max_permille[i] > 1000) {
            return validate_range_bool(false, "pwm_max_permille out of range", err_msg, err_len);
        }
        if (cfg->pwm_min_permille[i] > 1000) {
            return validate_range_bool(false, "pwm_min_permille out of range", err_msg, err_len);
        }
        if (cfg->pwm_min_permille[i] > cfg->pwm_max_permille[i]) {
            return validate_range_bool(false, "pwm_min_permille > pwm_max_permille", err_msg, err_len);
        }
    }
    return ESP_OK;
}

esp_err_t game_config_load(game_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(GAME_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    game_config_blob_t blob = {0};
    size_t len = sizeof(blob);
    err = nvs_get_blob(nvs, GAME_NVS_KEY_CFG, &blob, &len);
    nvs_close(nvs);

    if (err != ESP_OK) {
        return err;
    }
    if (blob.version != GAME_CONFIG_VERSION || len != sizeof(blob)) {
        return ESP_ERR_INVALID_VERSION;
    }
    *cfg = blob.cfg;
    return ESP_OK;
}

esp_err_t game_config_save(const game_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(GAME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    game_config_blob_t blob = {
        .version = GAME_CONFIG_VERSION,
        .cfg = *cfg,
    };

    err = nvs_set_blob(nvs, GAME_NVS_KEY_CFG, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t game_write_dac(game_engine_t *g, uint16_t code)
{
    if (!g || !g->hw.dac) {
        return ESP_OK;
    }
    esp_err_t err;
    if (g->hw.i2c_mutex) {
        xSemaphoreTake(g->hw.i2c_mutex, portMAX_DELAY);
    }
    err = dac7571_write(g->hw.dac, code, DAC7571_PD_NORMAL);
    if (g->hw.i2c_mutex) {
        xSemaphoreGive(g->hw.i2c_mutex);
    }
#ifdef CONFIG_APP_DEBUG_IO
    if (err == ESP_OK) {
        float voltage = (code / (float)DAC_MAX_CODE) * DAC_VREF_VOLTAGE;
        ESP_LOGI(TAG, "io: dac code=%u voltage=%.3fV", code, (double)voltage);
    }
#endif
    return err;
}

static void game_apply_outputs_off(game_engine_t *g, bool force)
{
    if (!g) {
        return;
    }
    if (g->hw.pwm) {
        for (int i = 0; i < 4; i++) {
            if (force || g->last_pwm_permille[i] != 0) {
                pwm_ledc_set_permille(g->hw.pwm, i, 0);
                g->last_pwm_permille[i] = 0;
            }
        }
    }
    if (force || g->last_ble_swing != 0) {
        ble_belt_send_swing(0);
        g->last_ble_swing = 0;
    }
    if (force || g->last_ble_vibrate != 0) {
        ble_belt_send_vibrate(0);
        g->last_ble_vibrate = 0;
    }
    if (force || g->last_dac_code != 0) {
        game_write_dac(g, 0);
        g->last_dac_code = 0;
    }
}

static void game_update_pressure_history(game_engine_t *g, float pressure)
{
    if (!g) {
        return;
    }
    if (g->pressure_hist_count < GAME_PRESSURE_HISTORY_LEN) {
        g->pressure_history[g->pressure_hist_index] = pressure;
        g->pressure_hist_sum += pressure;
        g->pressure_hist_count++;
        g->pressure_hist_index = (g->pressure_hist_index + 1) % GAME_PRESSURE_HISTORY_LEN;
    } else {
        float old = g->pressure_history[g->pressure_hist_index];
        g->pressure_hist_sum -= old;
        g->pressure_history[g->pressure_hist_index] = pressure;
        g->pressure_hist_sum += pressure;
        g->pressure_hist_index = (g->pressure_hist_index + 1) % GAME_PRESSURE_HISTORY_LEN;
    }
    if (g->pressure_hist_count > 0) {
        g->status.average_pressure = g->pressure_hist_sum / g->pressure_hist_count;
    } else {
        g->status.average_pressure = pressure;
    }
}

static void game_trigger_shock_locked(game_engine_t *g, bool force, int64_t now_ms)
{
    if (!g) {
        return;
    }
    if (!force && g->status.is_shocking) {
        return;
    }
    g->status.is_shocking = true;
    g->status.last_shock_time_ms = now_ms;
    g->status.shock_count += 1;
    ESP_LOGW(TAG, "shock triggered: %.2fV", g->config.shock_voltage);
}

static void game_update_shock_locked(game_engine_t *g, int64_t now_ms)
{
    if (!g) {
        return;
    }
    if (g->shock_end_time_ms <= 0) {
        return;
    }
    if (g->status.state == GAME_STATE_DELAY) {
        return;
    }
    if (now_ms >= g->shock_end_time_ms) {
        g->status.is_shocking = false;
        g->shock_end_time_ms = 0;
        ESP_LOGI(TAG, "shock ended");
    }
}

static void game_apply_outputs_locked(game_engine_t *g)
{
    if (!g) {
        return;
    }
    float ratio = 0.0f;
    if (g->config.max_motor_intensity > 0.0f) {
        ratio = g->status.current_intensity / g->config.max_motor_intensity;
    }
    ratio = clampf(ratio, 0.0f, 1.0f);

    if (g->hw.pwm) {
        for (int i = 0; i < 4; i++) {
            uint16_t max_permille = g->config.pwm_max_permille[i];
            uint16_t min_permille = g->config.pwm_min_permille[i];
            uint16_t permille = 0;
            if (ratio > 0.0f) {
                float span = (float)(max_permille - min_permille);
                float perm = (ratio * span) + (float)min_permille;
                permille = (uint16_t)(perm + 0.5f);
                if (permille < min_permille) permille = min_permille;
                if (permille > max_permille) permille = max_permille;
            }
            if (permille != g->last_pwm_permille[i]) {
                pwm_ledc_set_permille(g->hw.pwm, i, permille);
                g->last_pwm_permille[i] = permille;
#ifdef CONFIG_APP_DEBUG_IO
                ESP_LOGI(TAG, "io: pwm[%d]=%u", i, permille);
#endif
            }
        }
    }

    uint8_t swing = 0;
    if (ratio >= 0.85f) {
        swing = 3;
    } else if (ratio >= 0.60f) {
        swing = 2;
    } else if (ratio >= 0.25f) {
        swing = 1;
    } else {
        swing = 0;
    }
    uint8_t vibrate = (ratio > 0.0f) ? 3 : 0;

    if (swing != g->last_ble_swing) {
        ble_belt_send_swing(swing);
        g->last_ble_swing = swing;
#ifdef CONFIG_APP_DEBUG_IO
        ESP_LOGI(TAG, "io: ble swing=%u", swing);
#endif
    }
    if (vibrate != g->last_ble_vibrate) {
        ble_belt_send_vibrate(vibrate);
        g->last_ble_vibrate = vibrate;
#ifdef CONFIG_APP_DEBUG_IO
        ESP_LOGI(TAG, "io: ble vibrate=%u", vibrate);
#endif
    }

    uint16_t target_dac = 0;
    if (g->status.is_shocking) {
        target_dac = dac_code_from_voltage(g->config.shock_voltage);
    }
    if (target_dac != g->last_dac_code) {
        game_write_dac(g, target_dac);
        g->last_dac_code = target_dac;
    }
}

static void game_calculate_state_locked(game_engine_t *g, int64_t now_ms)
{
    float dt_sec = 0.0f;
    if (g->status.last_update_ms > 0) {
        dt_sec = (now_ms - g->status.last_update_ms) / 1000.0f;
        if (dt_sec < 0.0f) {
            dt_sec = 0.0f;
        }
    }
    g->status.last_update_ms = now_ms;

    const float pressure = g->status.current_pressure;
    const game_config_t *cfg = &g->config;

    switch (g->status.state) {
    case GAME_STATE_INITIAL_CALM: {
        float inc = dt_sec * (cfg->intensity_gradual_increase > 0 ? cfg->intensity_gradual_increase : 0.0f);
        g->status.unrandom_intensity += inc;
        float rnd = 1.0f + rand_unit() * (cfg->stimulation_ramp_random_percent / 100.0f);
        float target = g->status.unrandom_intensity * rnd;
        target = clampf(target, 0.0f, cfg->max_motor_intensity);
        g->status.target_intensity = target;

        if (pressure > cfg->mid_pressure_kpa) {
            g->status.recorded_mid_intensity = g->status.current_intensity;
            if (g->status.recorded_mid_intensity < 1.0f) {
                g->status.recorded_mid_intensity = g->status.target_intensity;
            }
            if (g->status.recorded_mid_intensity < 1.0f) {
                g->status.recorded_mid_intensity = cfg->max_motor_intensity * 0.5f;
            }
            g->status.state = GAME_STATE_MIDDLE;
            ESP_LOGI(TAG, "enter middle, recorded=%.2f", g->status.recorded_mid_intensity);
        }
        break;
    }
    case GAME_STATE_SUB_CALM: {
        float inc = dt_sec * (cfg->intensity_gradual_increase > 0 ? cfg->intensity_gradual_increase : 0.0f);
        g->status.unrandom_intensity += inc;
        float rnd = 1.0f + rand_unit() * (cfg->stimulation_ramp_random_percent / 100.0f);
        float target = g->status.unrandom_intensity * rnd;
        target = clampf(target, 0.0f, cfg->max_motor_intensity);
        g->status.target_intensity = target;

        if (pressure > cfg->mid_pressure_kpa) {
            g->status.recorded_mid_intensity = g->status.current_intensity;
            g->status.state = GAME_STATE_MIDDLE;
            ESP_LOGI(TAG, "enter middle (sub calm)");
        }
        break;
    }
    case GAME_STATE_MIDDLE: {
        g->status.mid_intensity = g->status.recorded_mid_intensity;
        float denominator = cfg->critical_pressure_kpa - cfg->mid_pressure_kpa;
        if (denominator < 0.01f) {
            denominator = 0.01f;
        }
        float factor = (cfg->critical_pressure_kpa - pressure) / denominator;
        float target = g->status.recorded_mid_intensity * (factor > 0.0f ? factor : 0.0f);
        float floor_intensity = cfg->mid_min_intensity;
        if (floor_intensity > g->status.recorded_mid_intensity) {
            floor_intensity = g->status.recorded_mid_intensity;
        }
        if (pressure < cfg->critical_pressure_kpa && target < floor_intensity) {
            target = floor_intensity;
        }
        g->status.target_intensity = clampf(target, 0.0f, cfg->max_motor_intensity);

        if (pressure >= cfg->critical_pressure_kpa) {
            g->status.state = GAME_STATE_EDGING;
            g->status.total_denied_times++;
            g->status.edging_count++;
            ESP_LOGW(TAG, "overload -> edging");
        } else if (pressure < cfg->mid_pressure_kpa) {
            g->status.unrandom_intensity = g->status.current_intensity;
            g->status.state = GAME_STATE_SUB_CALM;
            ESP_LOGI(TAG, "back to sub calm");
        }
        break;
    }
    case GAME_STATE_EDGING: {
        g->status.target_intensity = 0.0f;
        if (pressure < cfg->mid_pressure_kpa) {
            g->status.state = GAME_STATE_DELAY;
            g->status.state_timer_ms = now_ms;
            if (!g->status.is_shocking) {
                game_trigger_shock_locked(g, false, now_ms);
            }
            g->shock_end_time_ms = 0;
            ESP_LOGI(TAG, "enter delay");
        }
        break;
    }
    case GAME_STATE_DELAY: {
        g->status.target_intensity = 0.0f;
        if (pressure >= cfg->critical_pressure_kpa) {
            g->status.state = GAME_STATE_EDGING;
            g->status.is_shocking = false;
            g->shock_end_time_ms = 0;
            ESP_LOGW(TAG, "delay -> edging (critical)");
        } else if (pressure >= cfg->mid_pressure_kpa) {
            g->status.state = GAME_STATE_EDGING;
            g->status.is_shocking = false;
            g->shock_end_time_ms = 0;
            ESP_LOGI(TAG, "delay reset (pressure back)");
        } else if (now_ms - g->status.state_timer_ms > (int64_t)(cfg->low_pressure_delay_s * 1000.0f)) {
            float denom = cfg->critical_pressure_kpa - cfg->pressure_sensitivity;
            if (denom < 1e-6f) {
                denom = 1e-6f;
            }
            float base_target = cfg->max_motor_intensity * (cfg->critical_pressure_kpa - pressure) / denom;
            if (base_target < 0.0f) {
                base_target = 0.0f;
            }
            g->status.unrandom_intensity = base_target;
            g->status.state = GAME_STATE_SUB_CALM;
            g->status.is_shocking = false;
            g->shock_end_time_ms = 0;
            ESP_LOGI(TAG, "delay done -> sub calm");
        }
        break;
    }
    default:
        break;
    }

    g->status.prev_pressure = pressure;
}

static void game_update_intensity_locked(game_engine_t *g, int64_t now_ms)
{
    if (!g) {
        return;
    }
    float dt_sec = 0.0f;
    if (g->status.last_intensity_update_ms > 0) {
        dt_sec = (now_ms - g->status.last_intensity_update_ms) / 1000.0f;
        if (dt_sec < 0.0f) {
            dt_sec = 0.0f;
        }
    }
    g->status.last_intensity_update_ms = now_ms;

    float cur = g->status.current_intensity;
    float tgt = g->status.target_intensity;
    float next = cur;

    if (tgt < cur) {
        next = tgt;
    } else {
        float max_change = g->config.stimulation_ramp_rate_limit * dt_sec;
        if (max_change < 0.0f) {
            max_change = 0.0f;
        }
        next = cur + max_change;
        if (next > tgt) {
            next = tgt;
        }
    }

    g->status.current_intensity = next;
    if (next > 0.0f) {
        g->status.total_stimulation_time_s += dt_sec;
    }
}

esp_err_t game_engine_init(game_engine_t *g,
                           const game_engine_hw_t *hw,
                           const game_config_t *initial_cfg)
{
    if (!g || !hw || !initial_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(g, 0, sizeof(*g));
    g->hw = *hw;
    g->lock = xSemaphoreCreateMutex();
    if (!g->lock) {
        return ESP_ERR_NO_MEM;
    }
    g->config = *initial_cfg;
    g->status.running = false;
    g->status.paused = false;
    g->status.state = GAME_STATE_INITIAL_CALM;
    g->status.min_pressure = 999.0f;
    g->last_ble_swing = 0;
    g->last_ble_vibrate = 0;
    g->last_dac_code = 0;
    for (int i = 0; i < 4; i++) {
        g->last_pwm_permille[i] = 0;
    }
    return ESP_OK;
}

void game_engine_get_config(game_engine_t *g, game_config_t *out)
{
    if (!g || !out) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *out = g->config;
    xSemaphoreGive(g->lock);
}

esp_err_t game_engine_set_config(game_engine_t *g, const game_config_t *cfg)
{
    if (!g || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    g->config = *cfg;
    xSemaphoreGive(g->lock);
    return ESP_OK;
}

void game_engine_get_status(game_engine_t *g, game_status_t *out)
{
    if (!g || !out) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *out = g->status;
    xSemaphoreGive(g->lock);
}

bool game_engine_is_running(game_engine_t *g)
{
    if (!g) {
        return false;
    }
    bool running = false;
    if (xSemaphoreTake(g->lock, portMAX_DELAY) == pdTRUE) {
        running = g->status.running;
        xSemaphoreGive(g->lock);
    }
    return running;
}

void game_engine_start(game_engine_t *g, int64_t now_ms)
{
    if (!g) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    g->status.running = true;
    g->status.paused = false;
    g->status.start_time_ms = now_ms;
    g->status.end_time_ms = now_ms + (int64_t)(g->config.duration_min * 60.0f * 1000.0f);

    g->status.state = GAME_STATE_INITIAL_CALM;
    g->status.state_timer_ms = 0;
    g->status.recorded_mid_intensity = 0.0f;

    g->status.unrandom_intensity = 0.0f;
    g->status.target_intensity = 0.0f;
    g->status.current_intensity = 0.0f;
    g->status.mid_intensity = 0.5f * g->config.max_motor_intensity;
    g->status.last_update_ms = now_ms;
    g->status.last_intensity_update_ms = now_ms;
    g->status.prev_pressure = 0.0f;

    g->status.total_denied_times = 0;
    g->status.edging_count = 0;
    g->status.total_stimulation_time_s = 0.0f;

    g->status.is_shocking = false;
    g->status.last_shock_time_ms = 0;
    g->status.shock_count = 0;
    g->shock_end_time_ms = 0;
    g->shock_end_time_ms = 0;

    g->status.max_pressure = 0.0f;
    g->status.min_pressure = 999.0f;
    g->pressure_hist_count = 0;
    g->pressure_hist_index = 0;
    g->pressure_hist_sum = 0.0f;

    game_apply_outputs_off(g, true);
    xSemaphoreGive(g->lock);
}

void game_engine_stop(game_engine_t *g)
{
    if (!g) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    g->status.running = false;
    g->status.paused = false;
    g->status.is_shocking = false;
    game_apply_outputs_off(g, true);
    g->shock_end_time_ms = 0;
    xSemaphoreGive(g->lock);
}

void game_engine_set_paused(game_engine_t *g, bool paused)
{
    if (!g) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    g->status.paused = paused;
    xSemaphoreGive(g->lock);
}

void game_engine_trigger_shock(game_engine_t *g, bool force)
{
    if (!g) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (force) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        game_trigger_shock_locked(g, true, now_ms);
        g->shock_end_time_ms = now_ms + SHOCK_ONCE_MS;
    }
    game_apply_outputs_locked(g);
    xSemaphoreGive(g->lock);
}

void game_engine_on_sample(game_engine_t *g, float pressure_kpa, int64_t now_ms)
{
    if (!g) {
        return;
    }
    if (xSemaphoreTake(g->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    g->status.current_pressure = pressure_kpa;
    if (pressure_kpa > g->status.max_pressure) {
        g->status.max_pressure = pressure_kpa;
    }
    if (pressure_kpa < g->status.min_pressure) {
        g->status.min_pressure = pressure_kpa;
    }
    game_update_pressure_history(g, pressure_kpa);

    if (g->status.running && !g->status.paused) {
        if (now_ms >= g->status.end_time_ms) {
            game_apply_outputs_off(g, true);
            g->status.running = false;
            g->status.paused = false;
            g->status.is_shocking = false;
            g->shock_end_time_ms = 0;
            xSemaphoreGive(g->lock);
            return;
        }

        game_calculate_state_locked(g, now_ms);
        game_update_intensity_locked(g, now_ms);
        game_update_shock_locked(g, now_ms);
        game_apply_outputs_locked(g);
    }

    xSemaphoreGive(g->lock);
}
