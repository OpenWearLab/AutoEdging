#include "dglab_socket.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#define DGLAB_NVS_NAMESPACE "dglab"
#define DGLAB_NVS_KEY_CFG   "cfg"
#define DGLAB_MAX_CONNECT_FAILURES 3

static const char *TAG = "dglab_socket";

static void ws_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data);

typedef struct {
    uint32_t version;
    dglab_config_t cfg;
} dglab_config_blob_t;

#include "dglab_waveforms.h"

static void dglab_update_qr_text_locked(dglab_socket_t *svc)
{
    if (!svc) {
        return;
    }
    svc->status.qr_text[0] = '\0';
    if (svc->config.server_url[0] == '\0' || svc->status.client_id[0] == '\0') {
        return;
    }
    snprintf(svc->status.qr_text,
             sizeof(svc->status.qr_text),
             "https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#%s/%s",
             svc->config.server_url,
             svc->status.client_id);
}

static void dglab_set_state_locked(dglab_socket_t *svc, dglab_connection_state_t state)
{
    if (!svc) {
        return;
    }
    svc->status.connection_state = state;
}

static void dglab_reset_connect_failures_locked(dglab_socket_t *svc)
{
    if (!svc) {
        return;
    }
    svc->status.connect_fail_count = 0;
    svc->status.auto_disabled = false;
}

static void dglab_set_error_locked(dglab_socket_t *svc, const char *code, const char *text)
{
    if (!svc) {
        return;
    }
    snprintf(svc->status.last_error_code, sizeof(svc->status.last_error_code), "%s", code ? code : "");
    snprintf(svc->status.last_error_text, sizeof(svc->status.last_error_text), "%s", text ? text : "");
    if (svc->status.auto_disabled || svc->config.server_url[0] == '\0') {
        dglab_set_state_locked(svc, DGLAB_CONNECTION_DISABLED);
    } else {
        dglab_set_state_locked(svc, DGLAB_CONNECTION_ERROR);
    }
}

static void dglab_clear_pairing_locked(dglab_socket_t *svc)
{
    if (!svc) {
        return;
    }
    svc->status.paired = false;
    svc->status.target_id[0] = '\0';
    if (svc->status.auto_disabled || svc->config.server_url[0] == '\0') {
        dglab_set_state_locked(svc, DGLAB_CONNECTION_DISABLED);
    } else if (svc->status.websocket_connected) {
        dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTED);
    } else {
        dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTING);
    }
}

static void dglab_disconnect_locked(dglab_socket_t *svc, bool clear_client)
{
    if (!svc) {
        return;
    }
    svc->status.websocket_connected = false;
    svc->status.last_heartbeat_ms = 0;
    dglab_clear_pairing_locked(svc);
    if (clear_client) {
        svc->status.client_id[0] = '\0';
    }
    dglab_update_qr_text_locked(svc);
}

static void dglab_handle_connect_failure_locked(dglab_socket_t *svc)
{
    if (!svc || svc->config.server_url[0] == '\0' || svc->status.auto_disabled) {
        return;
    }

    if (svc->status.connect_fail_count < UINT8_MAX) {
        svc->status.connect_fail_count++;
    }

    if (svc->status.connect_fail_count < DGLAB_MAX_CONNECT_FAILURES) {
        char text[DGLAB_ERROR_TEXT_MAX_LEN];
        snprintf(text,
                 sizeof(text),
                 "connect failed (%u/%u)",
                 (unsigned)svc->status.connect_fail_count,
                 DGLAB_MAX_CONNECT_FAILURES);
        dglab_set_error_locked(svc, "connect_fail", text);
        return;
    }

    svc->status.auto_disabled = true;
    svc->status.websocket_connected = false;
    svc->status.last_heartbeat_ms = 0;
    svc->status.client_id[0] = '\0';
    svc->status.paired = false;
    svc->status.target_id[0] = '\0';
    dglab_update_qr_text_locked(svc);
    snprintf(svc->status.last_error_code, sizeof(svc->status.last_error_code), "%s", "auto_disabled");
    snprintf(svc->status.last_error_text,
             sizeof(svc->status.last_error_text),
             "disabled after %u failed connections",
             DGLAB_MAX_CONNECT_FAILURES);
    dglab_set_state_locked(svc, DGLAB_CONNECTION_DISABLED);
    if (svc->client) {
        esp_websocket_client_destroy_on_exit((esp_websocket_client_handle_t)svc->client);
        svc->client = NULL;
    }
}

static esp_err_t dglab_normalize_server_url(const char *input, char *out, size_t out_len, char *err_msg, size_t err_len)
{
    if (!input || !out || out_len == 0) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "serverUrl invalid");
        }
        return ESP_ERR_INVALID_ARG;
    }

    while (isspace((unsigned char)*input)) {
        input++;
    }

    size_t len = strlen(input);
    while (len > 0 && isspace((unsigned char)input[len - 1])) {
        len--;
    }

    if (len == 0) {
        out[0] = '\0';
        return ESP_OK;
    }

    const char *scheme = NULL;
    if (strncmp(input, "ws://", 5) == 0) {
        scheme = "ws://";
    } else if (strncmp(input, "wss://", 6) == 0) {
        scheme = "wss://";
    }
    if (!scheme) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "serverUrl must start with ws:// or wss://");
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= out_len) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "serverUrl too long");
        }
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(out, input, len);
    out[len] = '\0';
    while (len > strlen(scheme) && out[len - 1] == '/') {
        out[--len] = '\0';
    }

    const char *host = out + strlen(scheme);
    if (*host == '\0') {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "serverUrl missing host");
        }
        return ESP_ERR_INVALID_ARG;
    }

    const char *slash = strchr(host, '/');
    if (slash) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "serverUrl must not include path");
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (strchr(host, '#') || strchr(host, '?')) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "serverUrl must not include query");
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void dglab_config_set_defaults(dglab_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

esp_err_t dglab_config_validate(const dglab_config_t *cfg, char *err_msg, size_t err_len)
{
    if (!cfg) {
        if (err_msg && err_len > 0) {
            snprintf(err_msg, err_len, "cfg null");
        }
        return ESP_ERR_INVALID_ARG;
    }
    char normalized[DGLAB_SERVER_URL_MAX_LEN] = {0};
    return dglab_normalize_server_url(cfg->server_url, normalized, sizeof(normalized), err_msg, err_len);
}

esp_err_t dglab_config_load(dglab_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(DGLAB_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    dglab_config_blob_t blob = {0};
    size_t len = sizeof(blob);
    err = nvs_get_blob(nvs, DGLAB_NVS_KEY_CFG, &blob, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (blob.version != DGLAB_CONFIG_VERSION || len != sizeof(blob)) {
        return ESP_ERR_INVALID_VERSION;
    }
    *cfg = blob.cfg;
    return ESP_OK;
}

esp_err_t dglab_config_save(const dglab_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(DGLAB_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    dglab_config_blob_t blob = {
        .version = DGLAB_CONFIG_VERSION,
        .cfg = *cfg,
    };
    err = nvs_set_blob(nvs, DGLAB_NVS_KEY_CFG, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

const char *dglab_connection_state_to_string(dglab_connection_state_t state)
{
    switch (state) {
    case DGLAB_CONNECTION_DISABLED:
        return "disabled";
    case DGLAB_CONNECTION_CONNECTING:
        return "connecting";
    case DGLAB_CONNECTION_CONNECTED:
        return "connected";
    case DGLAB_CONNECTION_PAIRED:
        return "paired";
    case DGLAB_CONNECTION_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static esp_err_t dglab_send_json_locked(dglab_socket_t *svc, cJSON *root)
{
    if (!svc || !root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!svc->client || !svc->status.websocket_connected || !svc->status.paired) {
        return ESP_ERR_INVALID_STATE;
    }
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_websocket_client_send_text((esp_websocket_client_handle_t)svc->client,
                                                   payload,
                                                   strlen(payload),
                                                   pdMS_TO_TICKS(1000));
    free(payload);
    return err >= 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t dglab_start_client_locked(dglab_socket_t *svc)
{
    if (!svc) {
        return ESP_ERR_INVALID_ARG;
    }
    if (svc->config.server_url[0] == '\0') {
        dglab_reset_connect_failures_locked(svc);
        dglab_disconnect_locked(svc, true);
        dglab_set_state_locked(svc, DGLAB_CONNECTION_DISABLED);
        return ESP_OK;
    }
    if (svc->status.auto_disabled) {
        dglab_set_state_locked(svc, DGLAB_CONNECTION_DISABLED);
        return ESP_OK;
    }
    if (svc->client) {
        return esp_websocket_client_start((esp_websocket_client_handle_t)svc->client);
    }

    esp_websocket_client_config_t cfg = {
        .uri = svc->config.server_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 5000,
        .task_stack = 6144,
        .disable_auto_reconnect = false,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        dglab_set_error_locked(svc, "init", "ws init failed");
        return ESP_FAIL;
    }
    svc->client = client;
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, svc),
                        TAG,
                        "register ws events failed");
    dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTING);
    return esp_websocket_client_start(client);
}

static esp_err_t dglab_stop_client_locked(dglab_socket_t *svc)
{
    if (!svc || !svc->client) {
        return ESP_OK;
    }
    esp_websocket_client_handle_t client = (esp_websocket_client_handle_t)svc->client;
    if (esp_websocket_client_is_connected(client)) {
        esp_websocket_client_stop(client);
    }
    esp_websocket_client_destroy(client);
    svc->client = NULL;
    return ESP_OK;
}

static void dglab_handle_json_message_locked(dglab_socket_t *svc, const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ESP_LOGW(TAG, "invalid websocket json");
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const cJSON *client_id = cJSON_GetObjectItem(root, "clientId");
    const cJSON *target_id = cJSON_GetObjectItem(root, "targetId");
    const cJSON *message = cJSON_GetObjectItem(root, "message");

    const char *type_str = cJSON_IsString(type) ? cJSON_GetStringValue(type) : NULL;
    const char *client_id_str = cJSON_IsString(client_id) ? cJSON_GetStringValue(client_id) : NULL;
    const char *target_id_str = cJSON_IsString(target_id) ? cJSON_GetStringValue(target_id) : NULL;
    const char *message_str = cJSON_IsString(message) ? cJSON_GetStringValue(message) : NULL;

    if (!type_str) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type_str, "bind") == 0) {
        if (client_id_str && (!target_id_str || target_id_str[0] == '\0')) {
            snprintf(svc->status.client_id, sizeof(svc->status.client_id), "%s", client_id_str);
            svc->status.target_id[0] = '\0';
            svc->status.paired = false;
            dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTED);
            dglab_update_qr_text_locked(svc);
        } else if (client_id_str && target_id_str && message_str && strcmp(message_str, "200") == 0) {
            snprintf(svc->status.client_id, sizeof(svc->status.client_id), "%s", client_id_str);
            snprintf(svc->status.target_id, sizeof(svc->status.target_id), "%s", target_id_str);
            svc->status.paired = true;
            dglab_set_state_locked(svc, DGLAB_CONNECTION_PAIRED);
            dglab_update_qr_text_locked(svc);
        } else if (message_str) {
            dglab_set_error_locked(svc, message_str, "bind failed");
        }
    } else if (strcmp(type_str, "break") == 0) {
        dglab_set_error_locked(svc, message_str ? message_str : "209", "peer disconnected");
        dglab_clear_pairing_locked(svc);
    } else if (strcmp(type_str, "error") == 0) {
        dglab_set_error_locked(svc, message_str ? message_str : "error", "server returned error");
    } else if (strcmp(type_str, "heartbeat") == 0) {
        svc->status.last_heartbeat_ms = esp_timer_get_time() / 1000;
        if (svc->status.paired) {
            dglab_set_state_locked(svc, DGLAB_CONNECTION_PAIRED);
        } else {
            dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTED);
        }
    }

    cJSON_Delete(root);
}

static void ws_event_handler(void *handler_args,
                             esp_event_base_t base,
                             int32_t event_id,
                             void *event_data)
{
    (void)base;
    dglab_socket_t *svc = (dglab_socket_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (!svc || !svc->lock) {
        return;
    }

    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        dglab_reset_connect_failures_locked(svc);
        svc->status.websocket_connected = true;
        svc->status.last_error_code[0] = '\0';
        svc->status.last_error_text[0] = '\0';
        dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTED);
        ESP_LOGI(TAG, "connected to %s", svc->config.server_url);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        if (svc->config.server_url[0] != '\0' && !svc->status.websocket_connected) {
            dglab_handle_connect_failure_locked(svc);
        }
        dglab_disconnect_locked(svc, true);
        if (svc->config.server_url[0] != '\0' && !svc->status.auto_disabled) {
            dglab_set_state_locked(svc, DGLAB_CONNECTION_CONNECTING);
        }
        ESP_LOGW(TAG, "disconnected from websocket server");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data && data->data_ptr && data->data_len > 0) {
            size_t len = (size_t)data->data_len;
            char *payload = calloc(1, len + 1);
            if (payload) {
                memcpy(payload, data->data_ptr, len);
                payload[len] = '\0';
                dglab_handle_json_message_locked(svc, payload);
                free(payload);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        dglab_set_error_locked(svc, "ws", "websocket transport error");
        break;
    default:
        break;
    }

    xSemaphoreGive(svc->lock);
}

esp_err_t dglab_socket_init(dglab_socket_t *svc, const dglab_config_t *initial_cfg)
{
    if (!svc || !initial_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(svc, 0, sizeof(*svc));
    svc->lock = xSemaphoreCreateMutex();
    if (!svc->lock) {
        return ESP_ERR_NO_MEM;
    }
    svc->config = *initial_cfg;
    snprintf(svc->status.server_url, sizeof(svc->status.server_url), "%s", initial_cfg->server_url);
    svc->status.connection_state = initial_cfg->server_url[0] ? DGLAB_CONNECTION_CONNECTING : DGLAB_CONNECTION_DISABLED;

    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = dglab_start_client_locked(svc);
    xSemaphoreGive(svc->lock);
    return err;
}

esp_err_t dglab_socket_set_config(dglab_socket_t *svc, const dglab_config_t *cfg)
{
    if (!svc || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[DGLAB_SERVER_URL_MAX_LEN] = {0};
    char err_msg[96] = {0};
    ESP_RETURN_ON_ERROR(dglab_normalize_server_url(cfg->server_url, normalized, sizeof(normalized), err_msg, sizeof(err_msg)),
                        TAG,
                        "%s",
                        err_msg);

    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    bool changed = strcmp(svc->config.server_url, normalized) != 0;
    snprintf(svc->config.server_url, sizeof(svc->config.server_url), "%s", normalized);
    snprintf(svc->status.server_url, sizeof(svc->status.server_url), "%s", normalized);
    dglab_reset_connect_failures_locked(svc);
    if (changed || svc->client == NULL) {
        dglab_stop_client_locked(svc);
        dglab_disconnect_locked(svc, true);
        svc->status.last_error_code[0] = '\0';
        svc->status.last_error_text[0] = '\0';
        esp_err_t err = dglab_start_client_locked(svc);
        if (err != ESP_OK) {
            xSemaphoreGive(svc->lock);
            return err;
        }
    }

    xSemaphoreGive(svc->lock);
    return ESP_OK;
}

esp_err_t dglab_socket_reconnect(dglab_socket_t *svc)
{
    if (!svc) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    dglab_stop_client_locked(svc);
    dglab_disconnect_locked(svc, true);
    dglab_reset_connect_failures_locked(svc);
    esp_err_t err = dglab_start_client_locked(svc);
    xSemaphoreGive(svc->lock);
    return err;
}

void dglab_socket_get_config(dglab_socket_t *svc, dglab_config_t *out)
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

void dglab_socket_get_status(dglab_socket_t *svc, dglab_status_t *out)
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

bool dglab_socket_is_ready(dglab_socket_t *svc)
{
    if (!svc) {
        return false;
    }
    bool ready = false;
    if (xSemaphoreTake(svc->lock, portMAX_DELAY) == pdTRUE) {
        ready = svc->status.websocket_connected && svc->status.paired;
        xSemaphoreGive(svc->lock);
    }
    return ready;
}

esp_err_t dglab_socket_send_punishment(dglab_socket_t *svc, const dglab_punishment_t *punishment)
{
    if (!svc || !punishment) {
        return ESP_ERR_INVALID_ARG;
    }
    if (punishment->shock_channel != 'A' && punishment->shock_channel != 'B') {
        return ESP_ERR_INVALID_ARG;
    }
    if (punishment->shock_strength > 200 || punishment->shock_duration_s == 0 ||
        punishment->shock_waveform_preset < 1 ||
        punishment->shock_waveform_preset > DGLAB_WAVEFORM_PRESET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *waveform = s_waveform_presets[punishment->shock_waveform_preset];
    int channel_num = punishment->shock_channel == 'A' ? 1 : 2;

    if (xSemaphoreTake(svc->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!svc->status.websocket_connected || !svc->status.paired) {
        dglab_set_error_locked(svc, "not_ready", "DG-LAB not paired");
        xSemaphoreGive(svc->lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        xSemaphoreGive(svc->lock);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "type", 4);
    cJSON_AddStringToObject(root, "clientId", svc->status.client_id);
    cJSON_AddStringToObject(root, "targetId", svc->status.target_id);
    char clear_msg[16];
    snprintf(clear_msg, sizeof(clear_msg), "clear-%d", channel_num);
    cJSON_AddStringToObject(root, "message", clear_msg);
    err = dglab_send_json_locked(svc, root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        dglab_set_error_locked(svc, "send", "clear send failed");
        xSemaphoreGive(svc->lock);
        return err;
    }

    root = cJSON_CreateObject();
    if (!root) {
        xSemaphoreGive(svc->lock);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "type", 3);
    cJSON_AddStringToObject(root, "clientId", svc->status.client_id);
    cJSON_AddStringToObject(root, "targetId", svc->status.target_id);
    cJSON_AddStringToObject(root, "message", "set channel");
    cJSON_AddNumberToObject(root, "channel", channel_num);
    cJSON_AddNumberToObject(root, "strength", punishment->shock_strength);
    err = dglab_send_json_locked(svc, root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        dglab_set_error_locked(svc, "send", "strength send failed");
        xSemaphoreGive(svc->lock);
        return err;
    }

    root = cJSON_CreateObject();
    if (!root) {
        xSemaphoreGive(svc->lock);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "clientMsg");
    cJSON_AddStringToObject(root, "clientId", svc->status.client_id);
    cJSON_AddStringToObject(root, "targetId", svc->status.target_id);
    cJSON_AddStringToObject(root, "channel", punishment->shock_channel == 'A' ? "A" : "B");
    cJSON_AddNumberToObject(root, "time", punishment->shock_duration_s);

    size_t pulse_msg_len = strlen(waveform) + 3;
    char *pulse_msg = malloc(pulse_msg_len);
    if (!pulse_msg) {
        cJSON_Delete(root);
        xSemaphoreGive(svc->lock);
        return ESP_ERR_NO_MEM;
    }
    snprintf(pulse_msg, pulse_msg_len, "%c:%s", punishment->shock_channel, waveform);
    cJSON_AddStringToObject(root, "message", pulse_msg);
    free(pulse_msg);
    err = dglab_send_json_locked(svc, root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        dglab_set_error_locked(svc, "send", "pulse send failed");
        xSemaphoreGive(svc->lock);
        return err;
    }

    svc->status.last_error_code[0] = '\0';
    svc->status.last_error_text[0] = '\0';
    xSemaphoreGive(svc->lock);
    return ESP_OK;
}
