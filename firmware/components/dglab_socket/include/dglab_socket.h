#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DGLAB_CONFIG_VERSION 1
#define DGLAB_SERVER_URL_MAX_LEN 128
#define DGLAB_ID_MAX_LEN 48
#define DGLAB_QR_TEXT_MAX_LEN 256
#define DGLAB_ERROR_CODE_MAX_LEN 16
#define DGLAB_ERROR_TEXT_MAX_LEN 96
#define DGLAB_WAVEFORM_PRESET_COUNT 16

typedef enum {
    DGLAB_CONNECTION_DISABLED = 0,
    DGLAB_CONNECTION_CONNECTING,
    DGLAB_CONNECTION_CONNECTED,
    DGLAB_CONNECTION_PAIRED,
    DGLAB_CONNECTION_ERROR,
} dglab_connection_state_t;

typedef struct {
    char server_url[DGLAB_SERVER_URL_MAX_LEN];
} dglab_config_t;

typedef struct {
    char server_url[DGLAB_SERVER_URL_MAX_LEN];
    dglab_connection_state_t connection_state;
    bool websocket_connected;
    bool paired;
    bool auto_disabled;
    char client_id[DGLAB_ID_MAX_LEN];
    char target_id[DGLAB_ID_MAX_LEN];
    char qr_text[DGLAB_QR_TEXT_MAX_LEN];
    char last_error_code[DGLAB_ERROR_CODE_MAX_LEN];
    char last_error_text[DGLAB_ERROR_TEXT_MAX_LEN];
    uint8_t connect_fail_count;
    int64_t last_heartbeat_ms;
} dglab_status_t;

typedef struct {
    char shock_channel;
    uint8_t shock_strength;
    uint8_t shock_duration_s;
    uint8_t shock_waveform_preset;
} dglab_punishment_t;

typedef struct dglab_socket dglab_socket_t;

struct dglab_socket {
    dglab_config_t config;
    dglab_status_t status;
    SemaphoreHandle_t lock;
    void *client;
};

void dglab_config_set_defaults(dglab_config_t *cfg);
esp_err_t dglab_config_validate(const dglab_config_t *cfg, char *err_msg, size_t err_len);
esp_err_t dglab_config_load(dglab_config_t *cfg);
esp_err_t dglab_config_save(const dglab_config_t *cfg);

esp_err_t dglab_socket_init(dglab_socket_t *svc, const dglab_config_t *initial_cfg);
esp_err_t dglab_socket_set_config(dglab_socket_t *svc, const dglab_config_t *cfg);
esp_err_t dglab_socket_reconnect(dglab_socket_t *svc);
void dglab_socket_get_config(dglab_socket_t *svc, dglab_config_t *out);
void dglab_socket_get_status(dglab_socket_t *svc, dglab_status_t *out);
bool dglab_socket_is_ready(dglab_socket_t *svc);

esp_err_t dglab_socket_send_punishment(dglab_socket_t *svc, const dglab_punishment_t *punishment);

const char *dglab_connection_state_to_string(dglab_connection_state_t state);

#ifdef __cplusplus
}
#endif
