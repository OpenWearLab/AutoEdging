#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "act_pwm_ledc.h"
#include "ble_belt.h"
#include "nipple_dome.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROL_CONFIG_VERSION 4

typedef struct {
    nipple_dome_direction_t mode;
    uint16_t duty_permille;
} control_nipple_dome_config_t;

typedef struct {
    float pressure_threshold_kpa;
    uint16_t pwm_permille[4];       // 0..1000
    uint8_t ble_swing;              // 0..10
    uint8_t ble_vibrate;            // 0..10
    control_nipple_dome_config_t nipple_dome;
    uint32_t sample_hz;             // sensor sampling rate
    uint32_t ws_hz;                 // websocket push rate
    uint32_t window_sec;            // chart window in seconds
    bool status_led_enabled;        // RGB status led enable
} control_config_t;

typedef struct {
    float pressure_kpa;
    float temp_c;
    uint8_t sensor_status;
    uint16_t pwm_permille[4];
    uint8_t ble_swing;
    uint8_t ble_vibrate;
    nipple_dome_status_t nipple_dome;
    uint32_t sample_hz;
    uint32_t ws_hz;
    uint32_t window_sec;
    bool status_led_enabled;
    int64_t timestamp_ms;
} control_status_t;

typedef struct {
    pwm_ledc_t *pwm;
    nipple_dome_t *nipple_dome;
} control_service_hw_t;

typedef struct control_service {
    control_config_t config;
    control_status_t status;
    control_service_hw_t hw;
    SemaphoreHandle_t lock;
} control_service_t;

void control_config_set_defaults(control_config_t *cfg);

// Returns ESP_OK if valid; on error, err_msg is filled.
esp_err_t control_config_validate(const control_config_t *cfg, char *err_msg, size_t err_len);

// NVS helpers (namespace: "control")
esp_err_t control_config_load(control_config_t *cfg);
esp_err_t control_config_save(const control_config_t *cfg);

// Outputs service
esp_err_t control_service_init(control_service_t *svc,
                              const control_service_hw_t *hw,
                              const control_config_t *initial_cfg);

esp_err_t control_service_set_config(control_service_t *svc, const control_config_t *cfg);

void control_service_get_config(control_service_t *svc, control_config_t *out);

void control_service_get_status(control_service_t *svc, control_status_t *out);

void control_service_update_sensor(control_service_t *svc,
                                   float pressure_kpa,
                                   float temp_c,
                                   uint8_t sensor_status,
                                   int64_t timestamp_ms);

#ifdef __cplusplus
}
#endif
