#include "control_api.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONTROL_NVS_NAMESPACE   "control"
#define CONTROL_NVS_KEY_CFG     "cfg"

#define CONTROL_PRESSURE_MIN_KPA   (-20.0f)
#define CONTROL_PRESSURE_MAX_KPA   (200.0f)
#define CONTROL_SAMPLE_HZ_MIN      (1)
#define CONTROL_SAMPLE_HZ_MAX      (100)
#define CONTROL_WS_HZ_MIN          (1)
#define CONTROL_WS_HZ_MAX          (20)
#define CONTROL_WINDOW_MIN_SEC     (30)
#define CONTROL_WINDOW_MAX_SEC     (120)
#define CONTROL_PWM_MIN            (0)
#define CONTROL_PWM_MAX            (1000)
#define CONTROL_DAC_MIN            (0)
#define CONTROL_DAC_MAX            (4095)
#define CONTROL_BLE_MIN            (0)
#define CONTROL_BLE_MAX            (10)

static const char *TAG = "control_api";

typedef struct {
    uint32_t version;
    control_config_t cfg;
} control_config_blob_t;

void control_config_set_defaults(control_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->pressure_threshold_kpa = 20.0f;
    cfg->dac_code = 0;
    cfg->dac_pd = DAC7571_PD_NORMAL;
    cfg->pwm_permille[0] = 0;
    cfg->pwm_permille[1] = 0;
    cfg->pwm_permille[2] = 0;
    cfg->pwm_permille[3] = 0;
    cfg->ble_swing = 0;
    cfg->ble_vibrate = 0;
    cfg->sample_hz = 25;
    cfg->ws_hz = 5;
    cfg->window_sec = 60;
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

esp_err_t control_config_validate(const control_config_t *cfg, char *err_msg, size_t err_len)
{
    if (!cfg) {
        return validate_range_bool(false, "cfg null", err_msg, err_len);
    }
    if (cfg->pressure_threshold_kpa < CONTROL_PRESSURE_MIN_KPA ||
        cfg->pressure_threshold_kpa > CONTROL_PRESSURE_MAX_KPA) {
        return validate_range_bool(false, "pressure_threshold_kpa out of range", err_msg, err_len);
    }
    if (cfg->sample_hz < CONTROL_SAMPLE_HZ_MIN || cfg->sample_hz > CONTROL_SAMPLE_HZ_MAX) {
        return validate_range_bool(false, "sample_hz out of range", err_msg, err_len);
    }
    if (cfg->ws_hz < CONTROL_WS_HZ_MIN || cfg->ws_hz > CONTROL_WS_HZ_MAX) {
        return validate_range_bool(false, "ws_hz out of range", err_msg, err_len);
    }
    if (cfg->window_sec < CONTROL_WINDOW_MIN_SEC || cfg->window_sec > CONTROL_WINDOW_MAX_SEC) {
        return validate_range_bool(false, "window_sec out of range", err_msg, err_len);
    }
    for (int i = 0; i < 4; i++) {
        if (cfg->pwm_permille[i] < CONTROL_PWM_MIN || cfg->pwm_permille[i] > CONTROL_PWM_MAX) {
            return validate_range_bool(false, "pwm_permille out of range", err_msg, err_len);
        }
    }
    if (cfg->dac_code < CONTROL_DAC_MIN || cfg->dac_code > CONTROL_DAC_MAX) {
        return validate_range_bool(false, "dac_code out of range", err_msg, err_len);
    }
    if ((int)cfg->dac_pd < 0 || (int)cfg->dac_pd > 3) {
        return validate_range_bool(false, "dac_pd out of range", err_msg, err_len);
    }
    if (cfg->ble_swing < CONTROL_BLE_MIN || cfg->ble_swing > CONTROL_BLE_MAX) {
        return validate_range_bool(false, "ble_swing out of range", err_msg, err_len);
    }
    if (cfg->ble_vibrate < CONTROL_BLE_MIN || cfg->ble_vibrate > CONTROL_BLE_MAX) {
        return validate_range_bool(false, "ble_vibrate out of range", err_msg, err_len);
    }
    return ESP_OK;
}

esp_err_t control_config_load(control_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CONTROL_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    control_config_blob_t blob = {0};
    size_t len = sizeof(blob);
    err = nvs_get_blob(nvs, CONTROL_NVS_KEY_CFG, &blob, &len);
    nvs_close(nvs);

    if (err != ESP_OK) {
        return err;
    }
    if (blob.version != CONTROL_CONFIG_VERSION || len != sizeof(blob)) {
        return ESP_ERR_INVALID_VERSION;
    }
    *cfg = blob.cfg;
    return ESP_OK;
}

esp_err_t control_config_save(const control_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(CONTROL_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    control_config_blob_t blob = {
        .version = CONTROL_CONFIG_VERSION,
        .cfg = *cfg,
    };

    err = nvs_set_blob(nvs, CONTROL_NVS_KEY_CFG, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t apply_outputs_locked(control_service_t *svc, const control_config_t *cfg)
{
    esp_err_t err = ESP_OK;

    if (svc->hw.dac) {
        if (svc->hw.i2c_mutex) {
            xSemaphoreTake(svc->hw.i2c_mutex, portMAX_DELAY);
        }
        err = dac7571_write(svc->hw.dac, cfg->dac_code, cfg->dac_pd);
        if (svc->hw.i2c_mutex) {
            xSemaphoreGive(svc->hw.i2c_mutex);
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    if (svc->hw.pwm) {
        for (int i = 0; i < 4; i++) {
            err = pwm_ledc_set_permille(svc->hw.pwm, i, cfg->pwm_permille[i]);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    err = ble_belt_send_swing(cfg->ble_swing);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ble_swing send failed: %s", esp_err_to_name(err));
    }
    err = ble_belt_send_vibrate(cfg->ble_vibrate);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ble_vibrate send failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

static void update_status_from_config(control_status_t *st, const control_config_t *cfg)
{
    st->dac_code = cfg->dac_code;
    st->dac_pd = cfg->dac_pd;
    st->dac_voltage = (cfg->dac_code / 4095.0f) * 3.3f;
    for (int i = 0; i < 4; i++) {
        st->pwm_permille[i] = cfg->pwm_permille[i];
    }
    st->ble_swing = cfg->ble_swing;
    st->ble_vibrate = cfg->ble_vibrate;
    st->sample_hz = cfg->sample_hz;
    st->ws_hz = cfg->ws_hz;
    st->window_sec = cfg->window_sec;
}

esp_err_t control_service_init(control_service_t *svc,
                              const control_service_hw_t *hw,
                              const control_config_t *initial_cfg)
{
    if (!svc || !hw || !initial_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(svc, 0, sizeof(*svc));
    svc->hw = *hw;
    svc->lock = xSemaphoreCreateMutex();
    if (!svc->lock) {
        return ESP_ERR_NO_MEM;
    }
    svc->config = *initial_cfg;
    update_status_from_config(&svc->status, initial_cfg);
    return apply_outputs_locked(svc, initial_cfg);
}

esp_err_t control_service_set_config(control_service_t *svc, const control_config_t *cfg)
{
    if (!svc || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    svc->config = *cfg;
    update_status_from_config(&svc->status, cfg);
    esp_err_t err = apply_outputs_locked(svc, cfg);
    xSemaphoreGive(svc->lock);
    return err;
}

void control_service_get_config(control_service_t *svc, control_config_t *out)
{
    if (!svc || !out) {
        return;
    }
    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *out = svc->config;
    xSemaphoreGive(svc->lock);
}

void control_service_get_status(control_service_t *svc, control_status_t *out)
{
    if (!svc || !out) {
        return;
    }
    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *out = svc->status;
    xSemaphoreGive(svc->lock);
}

void control_service_update_sensor(control_service_t *svc,
                                   float pressure_kpa,
                                   float temp_c,
                                   uint8_t sensor_status,
                                   int64_t timestamp_ms)
{
    if (!svc) {
        return;
    }
    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    svc->status.pressure_kpa = pressure_kpa;
    svc->status.temp_c = temp_c;
    svc->status.sensor_status = sensor_status;
    svc->status.timestamp_ms = timestamp_ms;
    xSemaphoreGive(svc->lock);
}
