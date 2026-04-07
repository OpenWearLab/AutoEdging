#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_timer.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "game_engine.h"
#include "wifi_service.h"

static const char *TAG = "web_server";

#define WS_MAX_CLIENTS 4
#define MAX_POST_BODY  2048
#define WS_PUSH_MSG_MAX 3072

static httpd_handle_t s_server = NULL;
static web_server_ctx_t s_ctx = {0};
static int s_ws_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_lock = NULL;
static TaskHandle_t s_ws_task = NULL;

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static cJSON *nipple_dome_status_to_json(const nipple_dome_status_t *st)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "mode", nipple_dome_mode_to_string(st->mode));
    cJSON_AddStringToObject(root, "direction", nipple_dome_direction_to_string(st->direction));
    cJSON_AddNumberToObject(root, "duty_permille", st->duty_permille);
    cJSON_AddBoolToObject(root, "auto_enabled", st->auto_enabled);
    cJSON_AddNumberToObject(root, "switch_period_ms", st->switch_period_ms);
    cJSON_AddNumberToObject(root, "last_switch_ms", (double)st->last_switch_ms);
    return root;
}

static void apply_game_outputs_to_status(control_status_t *st, const game_status_t *gst, const game_config_t *cfg)
{
    if (!st || !gst || !cfg || !gst->running) {
        return;
    }
    float ratio = 0.0f;
    if (cfg->max_motor_intensity > 0.0f) {
        ratio = gst->current_intensity / cfg->max_motor_intensity;
    }
    ratio = clampf(ratio, 0.0f, 1.0f);

    for (int i = 0; i < 4; i++) {
        uint16_t max_permille = cfg->pwm_max_permille[i];
        uint16_t min_permille = cfg->pwm_min_permille[i];
        uint16_t permille = 0;
        if (ratio > 0.0f) {
            float span = (float)(max_permille - min_permille);
            float perm = (ratio * span) + (float)min_permille;
            permille = (uint16_t)(perm + 0.5f);
            if (permille < min_permille) permille = min_permille;
            if (permille > max_permille) permille = max_permille;
        }
        st->pwm_permille[i] = permille;
    }

    if (ratio >= 0.85f) {
        st->ble_swing = 3;
    } else if (ratio >= 0.60f) {
        st->ble_swing = 2;
    } else if (ratio >= 0.25f) {
        st->ble_swing = 1;
    } else {
        st->ble_swing = 0;
    }
    st->ble_vibrate = (ratio > 0.0f) ? 3 : 0;
    st->nipple_dome = gst->nipple_dome;
    st->nipple_dome.auto_enabled = gst->nipple_dome.auto_enabled;
}

static void ws_clients_init(void)
{
    if (!s_ws_lock) {
        s_ws_lock = xSemaphoreCreateMutex();
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        s_ws_clients[i] = -1;
    }
}

static void ws_add_client(int fd)
{
    if (!s_ws_lock) {
        return;
    }
    if (xSemaphoreTake(s_ws_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i] == fd) {
            xSemaphoreGive(s_ws_lock);
            return;
        }
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i] == -1) {
            s_ws_clients[i] = fd;
            break;
        }
    }
    xSemaphoreGive(s_ws_lock);
}

static void ws_remove_client(int fd)
{
    if (!s_ws_lock) {
        return;
    }
    if (xSemaphoreTake(s_ws_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_ws_clients[i] == fd) {
            s_ws_clients[i] = -1;
            break;
        }
    }
    xSemaphoreGive(s_ws_lock);
}

static void ws_broadcast(const char *msg)
{
    if (!s_server || !msg || !s_ws_lock) {
        return;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = strlen(msg),
    };

    if (xSemaphoreTake(s_ws_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        int fd = s_ws_clients[i];
        if (fd < 0) {
            continue;
        }
        esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
        if (err != ESP_OK) {
            s_ws_clients[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_lock);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    free(payload);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send(req, "{\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON_AddStringToObject(root, "error", msg ? msg : "error");
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static cJSON *config_to_json(const control_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "pressure_threshold_kpa", cfg->pressure_threshold_kpa);
    cJSON_AddNumberToObject(root, "sample_hz", cfg->sample_hz);
    cJSON_AddNumberToObject(root, "ws_hz", cfg->ws_hz);
    cJSON_AddNumberToObject(root, "window_sec", cfg->window_sec);
    cJSON_AddBoolToObject(root, "status_led_enabled", cfg->status_led_enabled);

    cJSON *pwm = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(pwm, cJSON_CreateNumber(cfg->pwm_permille[i]));
    }
    cJSON_AddItemToObject(root, "pwm", pwm);

    cJSON *ble = cJSON_CreateObject();
    cJSON_AddNumberToObject(ble, "swing", cfg->ble_swing);
    cJSON_AddNumberToObject(ble, "vibrate", cfg->ble_vibrate);
    cJSON_AddItemToObject(root, "ble", ble);

    cJSON *nipple_dome = cJSON_CreateObject();
    cJSON_AddStringToObject(nipple_dome, "mode", nipple_dome_direction_to_string(cfg->nipple_dome.mode));
    cJSON_AddNumberToObject(nipple_dome, "duty_permille", cfg->nipple_dome.duty_permille);
    cJSON_AddItemToObject(root, "nipple_dome", nipple_dome);

    return root;
}

static cJSON *status_to_json(const control_status_t *st)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "pressure_kpa", st->pressure_kpa);
    cJSON_AddNumberToObject(root, "temp_c", st->temp_c);
    cJSON_AddNumberToObject(root, "sensor_status", st->sensor_status);
    cJSON_AddNumberToObject(root, "timestamp_ms", (double)st->timestamp_ms);
    cJSON_AddNumberToObject(root, "sample_hz", st->sample_hz);
    cJSON_AddNumberToObject(root, "ws_hz", st->ws_hz);
    cJSON_AddNumberToObject(root, "window_sec", st->window_sec);
    cJSON_AddBoolToObject(root, "status_led_enabled", st->status_led_enabled);

    cJSON *pwm = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(pwm, cJSON_CreateNumber(st->pwm_permille[i]));
    }
    cJSON_AddItemToObject(root, "pwm", pwm);

    cJSON *ble = cJSON_CreateObject();
    cJSON_AddNumberToObject(ble, "swing", st->ble_swing);
    cJSON_AddNumberToObject(ble, "vibrate", st->ble_vibrate);
    cJSON_AddItemToObject(root, "ble", ble);

    cJSON *nipple_dome = nipple_dome_status_to_json(&st->nipple_dome);
    if (!nipple_dome) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "nipple_dome", nipple_dome);

    return root;
}

static cJSON *wifi_status_to_json(const wifi_service_status_t *st)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    const char *state = "connecting";
    if (st->reboot_pending) {
        state = "rebooting";
    } else if (st->provisioning) {
        state = "provisioning";
    } else if (st->connected) {
        state = "connected";
    } else if (st->connect_failed) {
        state = "failed";
    } else if (!st->provisioned) {
        state = "unprovisioned";
    }

    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddBoolToObject(root, "provisioned", st->provisioned);
    cJSON_AddBoolToObject(root, "connected", st->connected);
    cJSON_AddBoolToObject(root, "provisioning", st->provisioning);
    cJSON_AddBoolToObject(root, "connect_failed", st->connect_failed);
    cJSON_AddBoolToObject(root, "reboot_pending", st->reboot_pending);
    cJSON_AddStringToObject(root, "service_name", st->service_name[0] ? st->service_name : "");
    cJSON_AddStringToObject(root, "ssid", st->ssid[0] ? st->ssid : "");
    cJSON_AddStringToObject(root, "ip_addr", st->ip_addr[0] ? st->ip_addr : "");
    return root;
}

static const char *game_state_to_string(game_state_t state)
{
    switch (state) {
    case GAME_STATE_INITIAL_CALM:
        return "INITIAL_CALM";
    case GAME_STATE_MIDDLE:
        return "MIDDLE";
    case GAME_STATE_EDGING:
        return "EDGING";
    case GAME_STATE_DELAY:
        return "DELAY";
    case GAME_STATE_SUB_CALM:
        return "SUB_CALM";
    default:
        return "UNKNOWN";
    }
}

static cJSON *game_config_to_json(const game_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "duration", cfg->duration_min);
    cJSON_AddNumberToObject(root, "criticalPressure", cfg->critical_pressure_kpa);
    cJSON_AddNumberToObject(root, "maxMotorIntensity", cfg->max_motor_intensity);
    cJSON_AddNumberToObject(root, "lowPressureDelay", cfg->low_pressure_delay_s);
    cJSON_AddNumberToObject(root, "stimulationRampRateLimit", cfg->stimulation_ramp_rate_limit);
    cJSON_AddNumberToObject(root, "pressureSensitivity", cfg->pressure_sensitivity);
    cJSON_AddNumberToObject(root, "stimulationRampRandomPercent", cfg->stimulation_ramp_random_percent);
    cJSON_AddNumberToObject(root, "stimulationRampRandomInterval", cfg->stimulation_ramp_random_interval_s);
    cJSON_AddNumberToObject(root, "intensityGradualIncrease", cfg->intensity_gradual_increase);
    char shock_channel[2] = {cfg->shock_channel, '\0'};
    cJSON_AddStringToObject(root, "shockChannel", shock_channel);
    cJSON_AddNumberToObject(root, "shockStrength", cfg->shock_strength);
    cJSON_AddNumberToObject(root, "shockDuration", cfg->shock_duration_s);
    cJSON_AddNumberToObject(root, "shockWaveformPreset", cfg->shock_waveform_preset);
    cJSON_AddNumberToObject(root, "midPressure", cfg->mid_pressure_kpa);
    cJSON_AddNumberToObject(root, "midMinIntensity", cfg->mid_min_intensity);
    cJSON_AddBoolToObject(root, "nippleDomeEnabled", cfg->nipple_dome_enabled);
    cJSON_AddNumberToObject(root, "nippleDomeMinPermille", cfg->nipple_dome_min_permille);
    cJSON_AddNumberToObject(root, "nippleDomeMaxPermille", cfg->nipple_dome_max_permille);
    cJSON_AddNumberToObject(root, "nippleDomeSwitchPeriodMs", cfg->nipple_dome_switch_period_ms);

    cJSON *pwm = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(pwm, cJSON_CreateNumber(cfg->pwm_max_permille[i]));
    }
    cJSON_AddItemToObject(root, "pwmMaxPermille", pwm);

    cJSON *pwm_min = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(pwm_min, cJSON_CreateNumber(cfg->pwm_min_permille[i]));
    }
    cJSON_AddItemToObject(root, "pwmMinPermille", pwm_min);

    return root;
}

static cJSON *game_status_to_json(const game_status_t *st)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "running", st->running);
    cJSON_AddBoolToObject(root, "paused", st->paused);
    cJSON_AddStringToObject(root, "state", game_state_to_string(st->state));
    cJSON_AddNumberToObject(root, "startTimeMs", (double)st->start_time_ms);
    cJSON_AddNumberToObject(root, "endTimeMs", (double)st->end_time_ms);
    cJSON_AddNumberToObject(root, "currentPressure", st->current_pressure);
    cJSON_AddNumberToObject(root, "averagePressure", st->average_pressure);
    cJSON_AddNumberToObject(root, "currentIntensity", st->current_intensity);
    cJSON_AddNumberToObject(root, "targetIntensity", st->target_intensity);
    cJSON_AddNumberToObject(root, "midIntensity", st->mid_intensity);
    cJSON_AddNumberToObject(root, "edgingCount", (double)st->edging_count);
    cJSON_AddNumberToObject(root, "shockCount", (double)st->shock_count);
    cJSON_AddNumberToObject(root, "totalStimulationTime", st->total_stimulation_time_s);
    cJSON_AddNumberToObject(root, "maxPressure", st->max_pressure);
    cJSON_AddNumberToObject(root, "minPressure", st->min_pressure);
    cJSON_AddBoolToObject(root, "isShocking", st->is_shocking);

    cJSON *nipple_dome = cJSON_CreateObject();
    if (!nipple_dome) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddBoolToObject(nipple_dome, "enabled", st->nipple_dome_enabled);
    cJSON_AddStringToObject(nipple_dome, "mode", nipple_dome_mode_to_string(st->nipple_dome.mode));
    cJSON_AddStringToObject(nipple_dome, "direction", nipple_dome_direction_to_string(st->nipple_dome.direction));
    cJSON_AddNumberToObject(nipple_dome, "dutyPermille", st->nipple_dome.duty_permille);
    cJSON_AddBoolToObject(nipple_dome, "autoEnabled", st->nipple_dome.auto_enabled);
    cJSON_AddNumberToObject(nipple_dome, "switchPeriodMs", st->nipple_dome.switch_period_ms);
    cJSON_AddNumberToObject(nipple_dome, "lastSwitchMs", (double)st->nipple_dome.last_switch_ms);
    cJSON_AddItemToObject(root, "nippleDome", nipple_dome);
    return root;
}

static cJSON *dglab_config_to_json(const dglab_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "serverUrl", cfg->server_url);
    return root;
}

static cJSON *dglab_status_to_json(const dglab_status_t *st)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "serverUrl", st->server_url);
    cJSON_AddStringToObject(root, "connectionState", dglab_connection_state_to_string(st->connection_state));
    cJSON_AddBoolToObject(root, "websocketConnected", st->websocket_connected);
    cJSON_AddBoolToObject(root, "paired", st->paired);
    cJSON_AddBoolToObject(root, "autoDisabled", st->auto_disabled);
    cJSON_AddStringToObject(root, "clientId", st->client_id);
    cJSON_AddStringToObject(root, "targetId", st->target_id);
    cJSON_AddStringToObject(root, "qrText", st->qr_text);
    cJSON_AddStringToObject(root, "lastErrorCode", st->last_error_code);
    cJSON_AddStringToObject(root, "lastErrorText", st->last_error_text);
    cJSON_AddNumberToObject(root, "connectFailCount", st->connect_fail_count);
    cJSON_AddNumberToObject(root, "lastHeartbeatMs", (double)st->last_heartbeat_ms);
    return root;
}

static bool json_get_number(cJSON *obj, const char *name, double *out)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (item && cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    return false;
}

static bool json_get_bool(cJSON *obj, const char *name, bool *out)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (item && cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    return false;
}

static bool json_get_string(cJSON *obj, const char *name, const char **out)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (item && cJSON_IsString(item)) {
        *out = cJSON_GetStringValue(item);
        return *out != NULL;
    }
    return false;
}

static bool parse_nipple_dome_direction(const char *value, nipple_dome_direction_t *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "stop") == 0) {
        *out = NIPPLE_DOME_DIRECTION_STOP;
        return true;
    }
    if (strcmp(value, "forward") == 0) {
        *out = NIPPLE_DOME_DIRECTION_FORWARD;
        return true;
    }
    if (strcmp(value, "reverse") == 0) {
        *out = NIPPLE_DOME_DIRECTION_REVERSE;
        return true;
    }
    if (strcmp(value, "brake") == 0) {
        *out = NIPPLE_DOME_DIRECTION_BRAKE;
        return true;
    }
    return false;
}

static esp_err_t parse_config_body(const char *body,
                                  control_config_t *cfg,
                                  bool *save,
                                  bool *reset,
                                  char *err_msg,
                                  size_t err_len)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        snprintf(err_msg, err_len, "invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    bool save_local = true;
    bool reset_local = false;
    json_get_bool(root, "save", &save_local);
    json_get_bool(root, "reset", &reset_local);

    if (reset_local) {
        *reset = true;
        *save = save_local;
        cJSON_Delete(root);
        return ESP_OK;
    }

    double val = 0;
    if (json_get_number(root, "pressure_threshold_kpa", &val)) {
        cfg->pressure_threshold_kpa = (float)val;
    }
    if (json_get_number(root, "sample_hz", &val)) {
        cfg->sample_hz = (uint32_t)val;
    }
    if (json_get_number(root, "ws_hz", &val)) {
        cfg->ws_hz = (uint32_t)val;
    }
    if (json_get_number(root, "window_sec", &val)) {
        cfg->window_sec = (uint32_t)val;
    }
    bool status_led_enabled = cfg->status_led_enabled;
    if (json_get_bool(root, "status_led_enabled", &status_led_enabled)) {
        cfg->status_led_enabled = status_led_enabled;
    }

    cJSON *pwm = cJSON_GetObjectItem(root, "pwm");
    if (pwm && cJSON_IsArray(pwm)) {
        int len = cJSON_GetArraySize(pwm);
        for (int i = 0; i < len && i < 4; i++) {
            cJSON *item = cJSON_GetArrayItem(pwm, i);
            if (item && cJSON_IsNumber(item)) {
                cfg->pwm_permille[i] = (uint16_t)item->valuedouble;
            }
        }
    }

    cJSON *ble = cJSON_GetObjectItem(root, "ble");
    if (ble && cJSON_IsObject(ble)) {
        if (json_get_number(ble, "swing", &val)) {
            cfg->ble_swing = (uint8_t)val;
        }
        if (json_get_number(ble, "vibrate", &val)) {
            cfg->ble_vibrate = (uint8_t)val;
        }
    }

    cJSON *nipple_dome = cJSON_GetObjectItem(root, "nipple_dome");
    if (nipple_dome && cJSON_IsObject(nipple_dome)) {
        const char *mode = NULL;
        if (json_get_string(nipple_dome, "mode", &mode) && mode) {
            nipple_dome_direction_t direction = NIPPLE_DOME_DIRECTION_STOP;
            if (!parse_nipple_dome_direction(mode, &direction)) {
                snprintf(err_msg, err_len, "invalid nipple_dome.mode");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
            cfg->nipple_dome.mode = direction;
        }
        if (json_get_number(nipple_dome, "duty_permille", &val)) {
            cfg->nipple_dome.duty_permille = (uint16_t)val;
        }
    }

    *save = save_local;
    *reset = reset_local;
    cJSON_Delete(root);

    esp_err_t err = control_config_validate(cfg, err_msg, err_len);
    return err;
}

static esp_err_t parse_game_config_body(const char *body,
                                       game_config_t *cfg,
                                       bool *save,
                                       bool *reset,
                                       char *err_msg,
                                       size_t err_len)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        snprintf(err_msg, err_len, "invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    bool save_local = true;
    bool reset_local = false;
    json_get_bool(root, "save", &save_local);
    json_get_bool(root, "reset", &reset_local);

    if (reset_local) {
        *reset = true;
        *save = save_local;
        cJSON_Delete(root);
        return ESP_OK;
    }

    double val = 0;
    if (json_get_number(root, "duration", &val)) {
        cfg->duration_min = (float)val;
    }
    if (json_get_number(root, "criticalPressure", &val)) {
        cfg->critical_pressure_kpa = (float)val;
    }
    if (json_get_number(root, "maxMotorIntensity", &val)) {
        cfg->max_motor_intensity = (float)val;
    }
    if (json_get_number(root, "lowPressureDelay", &val)) {
        cfg->low_pressure_delay_s = (float)val;
    }
    if (json_get_number(root, "stimulationRampRateLimit", &val)) {
        cfg->stimulation_ramp_rate_limit = (float)val;
    }
    if (json_get_number(root, "pressureSensitivity", &val)) {
        cfg->pressure_sensitivity = (float)val;
    }
    if (json_get_number(root, "stimulationRampRandomPercent", &val)) {
        cfg->stimulation_ramp_random_percent = (float)val;
    }
    if (json_get_number(root, "stimulationRampRandomInterval", &val)) {
        cfg->stimulation_ramp_random_interval_s = (float)val;
    }
    if (json_get_number(root, "intensityGradualIncrease", &val)) {
        cfg->intensity_gradual_increase = (float)val;
    }
    const char *str_val = NULL;
    if (json_get_string(root, "shockChannel", &str_val) && str_val && (str_val[0] == 'A' || str_val[0] == 'B')) {
        cfg->shock_channel = str_val[0];
    }
    if (json_get_number(root, "shockStrength", &val)) {
        cfg->shock_strength = (uint8_t)val;
    }
    if (json_get_number(root, "shockDuration", &val)) {
        cfg->shock_duration_s = (uint8_t)val;
    }
    if (json_get_number(root, "shockWaveformPreset", &val)) {
        cfg->shock_waveform_preset = (uint8_t)val;
    }
    if (json_get_number(root, "midPressure", &val)) {
        cfg->mid_pressure_kpa = (float)val;
    }
    if (json_get_number(root, "midMinIntensity", &val)) {
        cfg->mid_min_intensity = (float)val;
    }
    bool nipple_dome_enabled = cfg->nipple_dome_enabled;
    if (json_get_bool(root, "nippleDomeEnabled", &nipple_dome_enabled)) {
        cfg->nipple_dome_enabled = nipple_dome_enabled;
    }
    if (json_get_number(root, "nippleDomeMinPermille", &val)) {
        cfg->nipple_dome_min_permille = (uint16_t)val;
    }
    if (json_get_number(root, "nippleDomeMaxPermille", &val)) {
        cfg->nipple_dome_max_permille = (uint16_t)val;
    }
    if (json_get_number(root, "nippleDomeSwitchPeriodMs", &val)) {
        cfg->nipple_dome_switch_period_ms = (uint32_t)val;
    }

    cJSON *pwm = cJSON_GetObjectItem(root, "pwmMaxPermille");
    if (pwm && cJSON_IsArray(pwm)) {
        int len = cJSON_GetArraySize(pwm);
        for (int i = 0; i < len && i < 4; i++) {
            cJSON *item = cJSON_GetArrayItem(pwm, i);
            if (item && cJSON_IsNumber(item)) {
                cfg->pwm_max_permille[i] = (uint16_t)item->valuedouble;
            }
        }
    }

    cJSON *pwm_min = cJSON_GetObjectItem(root, "pwmMinPermille");
    if (pwm_min && cJSON_IsArray(pwm_min)) {
        int len = cJSON_GetArraySize(pwm_min);
        for (int i = 0; i < len && i < 4; i++) {
            cJSON *item = cJSON_GetArrayItem(pwm_min, i);
            if (item && cJSON_IsNumber(item)) {
                cfg->pwm_min_permille[i] = (uint16_t)item->valuedouble;
            }
        }
    }

    *save = save_local;
    *reset = reset_local;
    cJSON_Delete(root);

    esp_err_t err = game_config_validate(cfg, err_msg, err_len);
    return err;
}

static esp_err_t parse_dglab_config_body(const char *body,
                                         dglab_config_t *cfg,
                                         bool *save,
                                         bool *reconnect,
                                         char *err_msg,
                                         size_t err_len)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        snprintf(err_msg, err_len, "invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    bool save_local = true;
    bool reconnect_local = false;
    json_get_bool(root, "save", &save_local);
    json_get_bool(root, "reconnect", &reconnect_local);

    const char *server_url = NULL;
    if (json_get_string(root, "serverUrl", &server_url) && server_url) {
        snprintf(cfg->server_url, sizeof(cfg->server_url), "%s", server_url);
    }

    *save = save_local;
    *reconnect = reconnect_local;
    cJSON_Delete(root);
    return dglab_config_validate(cfg, err_msg, err_len);
}

static esp_err_t handle_get_status(httpd_req_t *req)
{
    control_status_t st = {0};
    control_service_get_status(s_ctx.control, &st);
    if (s_ctx.game) {
        game_status_t gst = {0};
        game_config_t gcfg = {0};
        game_engine_get_status(s_ctx.game, &gst);
        game_engine_get_config(s_ctx.game, &gcfg);
        apply_game_outputs_to_status(&st, &gst, &gcfg);
    }

    cJSON *root = status_to_json(&st);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    control_config_t cfg = {0};
    control_service_get_config(s_ctx.control, &cfg);

    cJSON *root = config_to_json(&cfg);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_wifi_status(httpd_req_t *req)
{
    wifi_service_status_t st = {0};
    wifi_service_get_status(&st);

    cJSON *root = wifi_status_to_json(&st);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    if (s_ctx.game && game_engine_is_running(s_ctx.game)) {
        return send_error(req, "409 Conflict", "game running");
    }

    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        return send_error(req, "400 Bad Request", "invalid content length");
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return send_error(req, "400 Bad Request", "recv failed");
        }
        received += ret;
    }

    control_config_t cfg = {0};
    control_service_get_config(s_ctx.control, &cfg);

    bool save = true;
    bool reset = false;
    char err_msg[96] = {0};
    esp_err_t err = parse_config_body(buf, &cfg, &save, &reset, err_msg, sizeof(err_msg));
    free(buf);

    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", err_msg[0] ? err_msg : "invalid config");
    }

    if (reset) {
        control_config_set_defaults(&cfg);
    }

    err = control_service_set_config(s_ctx.control, &cfg);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "apply failed");
    }
    if (save) {
        err = control_config_save(&cfg);
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "save failed");
        }
    }

    cJSON *root = config_to_json(&cfg);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_game_status(httpd_req_t *req)
{
    if (!s_ctx.game) {
        return send_error(req, "500 Internal Server Error", "game not ready");
    }
    game_status_t st = {0};
    game_engine_get_status(s_ctx.game, &st);

    cJSON *root = game_status_to_json(&st);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_game_config(httpd_req_t *req)
{
    if (!s_ctx.game) {
        return send_error(req, "500 Internal Server Error", "game not ready");
    }
    game_config_t cfg = {0};
    game_engine_get_config(s_ctx.game, &cfg);

    cJSON *root = game_config_to_json(&cfg);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_dglab_config(httpd_req_t *req)
{
    if (!s_ctx.dglab) {
        return send_error(req, "500 Internal Server Error", "dglab not ready");
    }
    dglab_config_t cfg = {0};
    dglab_socket_get_config(s_ctx.dglab, &cfg);
    cJSON *root = dglab_config_to_json(&cfg);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_get_dglab_status(httpd_req_t *req)
{
    if (!s_ctx.dglab) {
        return send_error(req, "500 Internal Server Error", "dglab not ready");
    }
    dglab_status_t st = {0};
    dglab_socket_get_status(s_ctx.dglab, &st);
    cJSON *root = dglab_status_to_json(&st);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_post_game_config(httpd_req_t *req)
{
    if (!s_ctx.game) {
        return send_error(req, "500 Internal Server Error", "game not ready");
    }
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        return send_error(req, "400 Bad Request", "invalid content length");
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return send_error(req, "400 Bad Request", "recv failed");
        }
        received += ret;
    }

    game_config_t cfg = {0};
    game_engine_get_config(s_ctx.game, &cfg);

    bool save = true;
    bool reset = false;
    char err_msg[96] = {0};
    esp_err_t err = parse_game_config_body(buf, &cfg, &save, &reset, err_msg, sizeof(err_msg));
    free(buf);

    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", err_msg[0] ? err_msg : "invalid config");
    }

    if (reset) {
        game_config_set_defaults(&cfg);
    }

    err = game_engine_set_config(s_ctx.game, &cfg);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "apply failed");
    }
    if (save) {
        err = game_config_save(&cfg);
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "save failed");
        }
    }

    cJSON *root = game_config_to_json(&cfg);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_post_dglab_config(httpd_req_t *req)
{
    if (!s_ctx.dglab) {
        return send_error(req, "500 Internal Server Error", "dglab not ready");
    }
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        return send_error(req, "400 Bad Request", "invalid content length");
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return send_error(req, "400 Bad Request", "recv failed");
        }
        received += ret;
    }

    dglab_config_t cfg = {0};
    dglab_socket_get_config(s_ctx.dglab, &cfg);

    bool save = true;
    bool reconnect = false;
    char err_msg[96] = {0};
    esp_err_t err = parse_dglab_config_body(buf, &cfg, &save, &reconnect, err_msg, sizeof(err_msg));
    free(buf);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", err_msg[0] ? err_msg : "invalid dglab config");
    }

    err = dglab_socket_set_config(s_ctx.dglab, &cfg);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "apply failed");
    }
    if (save) {
        err = dglab_config_save(&cfg);
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "save failed");
        }
    }
    if (reconnect) {
        err = dglab_socket_reconnect(s_ctx.dglab);
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "reconnect failed");
        }
    }

    dglab_status_t st = {0};
    dglab_socket_get_status(s_ctx.dglab, &st);
    cJSON *root = dglab_status_to_json(&st);
    if (!root) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_post_wifi_action(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        return send_error(req, "400 Bad Request", "invalid content length");
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return send_error(req, "400 Bad Request", "recv failed");
        }
        received += ret;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return send_error(req, "400 Bad Request", "invalid JSON");
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "missing action");
    }

    const char *act = cJSON_GetStringValue(action);
    if (!act || strcmp(act, "reprovision_reboot") != 0) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "unknown action");
    }

    esp_err_t err = wifi_service_request_reprovision_reboot();
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "reprovision request failed");
    }

    wifi_service_status_t st = {0};
    wifi_service_get_status(&st);

    cJSON *resp = wifi_status_to_json(&st);
    if (!resp) {
        return send_error(req, "500 Internal Server Error", "json alloc failed");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "wifi credentials cleared, rebooting");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_post_game_control(httpd_req_t *req)
{
    if (!s_ctx.game) {
        return send_error(req, "500 Internal Server Error", "game not ready");
    }
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        return send_error(req, "400 Bad Request", "invalid content length");
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "oom");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            return send_error(req, "400 Bad Request", "recv failed");
        }
        received += ret;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return send_error(req, "400 Bad Request", "invalid JSON");
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "missing action");
    }

    const char *act = cJSON_GetStringValue(action);
    int64_t now_ms = 0;
    if (strcmp(act, "start") == 0) {
        now_ms = esp_timer_get_time() / 1000;
        game_engine_start(s_ctx.game, now_ms);
    } else if (strcmp(act, "stop") == 0) {
        game_engine_stop(s_ctx.game);
    } else if (strcmp(act, "pause") == 0) {
        game_status_t st = {0};
        game_engine_get_status(s_ctx.game, &st);
        game_engine_set_paused(s_ctx.game, !st.paused);
    } else if (strcmp(act, "shockOnce") == 0) {
        game_engine_trigger_shock(s_ctx.game, true);
    } else {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "unknown action");
    }

    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        return httpd_resp_send_404(req);
    }
    char buf[512];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t send_static_file(httpd_req_t *req, const char *path, const char *type)
{
    httpd_resp_set_type(req, type);
    FILE *f = fopen(path, "r");
    if (!f) {
        return httpd_resp_send_404(req);
    }
    char buf[512];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
            fclose(f);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    return send_static_file(req, "/spiffs/app.js", "application/javascript");
}

static esp_err_t handle_app_css(httpd_req_t *req)
{
    return send_static_file(req, "/spiffs/app.css", "text/css");
}

static esp_err_t handle_qrcode_js(httpd_req_t *req)
{
    return send_static_file(req, "/spiffs/qrcode.min.js", "application/javascript");
}

static esp_err_t handle_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        ws_remove_client(fd);
        return ret;
    }
    if (frame.len > 0) {
        uint8_t *buf = calloc(1, frame.len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        frame.payload = buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        free(buf);
        if (ret != ESP_OK) {
            int fd = httpd_req_to_sockfd(req);
            ws_remove_client(fd);
            return ret;
        }
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ws_remove_client(fd);
    }
    return ESP_OK;
}

static cJSON *ws_payload_to_json(const control_status_t *st,
                                 const dglab_status_t *dst,
                                 const game_status_t *gst)
{
    if (!st || !dst || !gst) {
        return NULL;
    }

    cJSON *root = status_to_json(st);
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "ts", (double)st->timestamp_ms);

    cJSON *dglab = dglab_status_to_json(dst);
    if (!dglab) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "dglab", dglab);

    cJSON *game = game_status_to_json(gst);
    if (!game) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "game", game);
    return root;
}

static void ws_push_task(void *arg)
{
    (void)arg;
    while (1) {
        control_config_t cfg = {0};
        control_service_get_config(s_ctx.control, &cfg);
        uint32_t ws_hz = cfg.ws_hz;
        if (ws_hz < 1) {
            ws_hz = 1;
        } else if (ws_hz > 20) {
            ws_hz = 20;
        }

        control_status_t st = {0};
        control_service_get_status(s_ctx.control, &st);
        telemetry_point_t tp = {0};
        if (s_ctx.telemetry && telemetry_get_latest(s_ctx.telemetry, &tp)) {
            st.timestamp_ms = tp.ts_ms;
            st.pressure_kpa = tp.pressure_kpa;
            st.temp_c = tp.temp_c;
        }
        game_status_t gst = {0};
        game_config_t gcfg = {0};
        dglab_status_t dst = {0};
        if (s_ctx.game) {
            game_engine_get_status(s_ctx.game, &gst);
            game_engine_get_config(s_ctx.game, &gcfg);
            apply_game_outputs_to_status(&st, &gst, &gcfg);
        }
        if (s_ctx.dglab) {
            dglab_socket_get_status(s_ctx.dglab, &dst);
        }

        cJSON *root = ws_payload_to_json(&st, &dst, &gst);
        if (root) {
            cJSON *game = cJSON_GetObjectItem(root, "game");
            if (game) {
                cJSON_AddNumberToObject(game, "midPressure", gcfg.mid_pressure_kpa);
                cJSON_AddNumberToObject(game, "criticalPressure", gcfg.critical_pressure_kpa);
            }
            char *msg = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (msg) {
                size_t len = strlen(msg);
                if (len < WS_PUSH_MSG_MAX) {
                    ws_broadcast(msg);
                } else {
                    ESP_LOGW(TAG, "ws push payload truncated: len=%u cap=%u",
                             (unsigned)len, (unsigned)WS_PUSH_MSG_MAX);
                }
                free(msg);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / ws_hz));
    }
}

esp_err_t web_server_start(const web_server_ctx_t *ctx)
{
    if (!ctx || !ctx->control || !ctx->dglab || !ctx->telemetry || !ctx->game) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ctx = *ctx;
    ws_clients_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = handle_get_status,
    };
    httpd_uri_t config_get_uri = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = handle_get_config,
    };
    httpd_uri_t config_post_uri = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = handle_post_config,
    };
    httpd_uri_t wifi_status_uri = {
        .uri = "/api/system/wifi",
        .method = HTTP_GET,
        .handler = handle_get_wifi_status,
    };
    httpd_uri_t wifi_action_uri = {
        .uri = "/api/system/wifi",
        .method = HTTP_POST,
        .handler = handle_post_wifi_action,
    };
    httpd_uri_t game_status_uri = {
        .uri = "/api/game/status",
        .method = HTTP_GET,
        .handler = handle_get_game_status,
    };
    httpd_uri_t game_config_get_uri = {
        .uri = "/api/game/config",
        .method = HTTP_GET,
        .handler = handle_get_game_config,
    };
    httpd_uri_t game_config_post_uri = {
        .uri = "/api/game/config",
        .method = HTTP_POST,
        .handler = handle_post_game_config,
    };
    httpd_uri_t dglab_config_get_uri = {
        .uri = "/api/dglab/config",
        .method = HTTP_GET,
        .handler = handle_get_dglab_config,
    };
    httpd_uri_t dglab_config_post_uri = {
        .uri = "/api/dglab/config",
        .method = HTTP_POST,
        .handler = handle_post_dglab_config,
    };
    httpd_uri_t dglab_status_uri = {
        .uri = "/api/dglab/status",
        .method = HTTP_GET,
        .handler = handle_get_dglab_status,
    };
    httpd_uri_t game_control_post_uri = {
        .uri = "/api/game/control",
        .method = HTTP_POST,
        .handler = handle_post_game_control,
    };
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws,
        .is_websocket = true,
    };
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
    };
    httpd_uri_t app_js_uri = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = handle_app_js,
    };
    httpd_uri_t app_css_uri = {
        .uri = "/app.css",
        .method = HTTP_GET,
        .handler = handle_app_css,
    };
    httpd_uri_t qrcode_js_uri = {
        .uri = "/qrcode.min.js",
        .method = HTTP_GET,
        .handler = handle_qrcode_js,
    };

    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &config_get_uri);
    httpd_register_uri_handler(s_server, &config_post_uri);
    httpd_register_uri_handler(s_server, &wifi_status_uri);
    httpd_register_uri_handler(s_server, &wifi_action_uri);
    httpd_register_uri_handler(s_server, &game_status_uri);
    httpd_register_uri_handler(s_server, &game_config_get_uri);
    httpd_register_uri_handler(s_server, &game_config_post_uri);
    httpd_register_uri_handler(s_server, &dglab_config_get_uri);
    httpd_register_uri_handler(s_server, &dglab_config_post_uri);
    httpd_register_uri_handler(s_server, &dglab_status_uri);
    httpd_register_uri_handler(s_server, &game_control_post_uri);
    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &app_js_uri);
    httpd_register_uri_handler(s_server, &app_css_uri);
    httpd_register_uri_handler(s_server, &qrcode_js_uri);

    if (!s_ws_task) {
        xTaskCreate(ws_push_task, "ws_push", 6144, NULL, 5, &s_ws_task);
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
