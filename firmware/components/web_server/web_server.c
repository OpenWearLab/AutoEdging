#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_timer.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "game_engine.h"

static const char *TAG = "web_server";

#define WS_MAX_CLIENTS 4
#define MAX_POST_BODY  2048

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

    float voltage = gst->is_shocking ? cfg->shock_voltage : 0.0f;
    voltage = clampf(voltage, 0.0f, 2.1f);
    uint16_t code = (uint16_t)((voltage / 3.3f) * 4095.0f + 0.5f);
    st->dac_code = code;
    st->dac_voltage = voltage;
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

    cJSON *dac = cJSON_CreateObject();
    cJSON_AddNumberToObject(dac, "code", cfg->dac_code);
    cJSON_AddNumberToObject(dac, "pd_mode", cfg->dac_pd);
    cJSON_AddNumberToObject(dac, "voltage", (cfg->dac_code / 4095.0f) * 3.3f);
    cJSON_AddItemToObject(root, "dac", dac);

    cJSON *pwm = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(pwm, cJSON_CreateNumber(cfg->pwm_permille[i]));
    }
    cJSON_AddItemToObject(root, "pwm", pwm);

    cJSON *ble = cJSON_CreateObject();
    cJSON_AddNumberToObject(ble, "swing", cfg->ble_swing);
    cJSON_AddNumberToObject(ble, "vibrate", cfg->ble_vibrate);
    cJSON_AddItemToObject(root, "ble", ble);

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

    cJSON *dac = cJSON_CreateObject();
    cJSON_AddNumberToObject(dac, "code", st->dac_code);
    cJSON_AddNumberToObject(dac, "pd_mode", st->dac_pd);
    cJSON_AddNumberToObject(dac, "voltage", st->dac_voltage);
    cJSON_AddItemToObject(root, "dac", dac);

    cJSON *pwm = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(pwm, cJSON_CreateNumber(st->pwm_permille[i]));
    }
    cJSON_AddItemToObject(root, "pwm", pwm);

    cJSON *ble = cJSON_CreateObject();
    cJSON_AddNumberToObject(ble, "swing", st->ble_swing);
    cJSON_AddNumberToObject(ble, "vibrate", st->ble_vibrate);
    cJSON_AddItemToObject(root, "ble", ble);

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
    cJSON_AddNumberToObject(root, "shockIntensity", cfg->shock_voltage);
    cJSON_AddNumberToObject(root, "midPressure", cfg->mid_pressure_kpa);
    cJSON_AddNumberToObject(root, "midMinIntensity", cfg->mid_min_intensity);

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

    cJSON *dac = cJSON_GetObjectItem(root, "dac");
    if (dac && cJSON_IsObject(dac)) {
        if (json_get_number(dac, "code", &val)) {
            cfg->dac_code = (uint16_t)val;
        } else if (json_get_number(dac, "voltage", &val)) {
            int code = (int)((val / 3.3f) * 4095.0f + 0.5f);
            if (code < 0) code = 0;
            if (code > 4095) code = 4095;
            cfg->dac_code = (uint16_t)code;
        }
        if (json_get_number(dac, "pd_mode", &val)) {
            cfg->dac_pd = (dac7571_pd_t)((int)val);
        }
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
    if (json_get_number(root, "shockIntensity", &val)) {
        cfg->shock_voltage = (float)val;
    }
    if (json_get_number(root, "midPressure", &val)) {
        cfg->mid_pressure_kpa = (float)val;
    }
    if (json_get_number(root, "midMinIntensity", &val)) {
        cfg->mid_min_intensity = (float)val;
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

static void ws_push_task(void *arg)
{
    (void)arg;
    char msg[768];

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
        if (s_ctx.game) {
            game_engine_get_status(s_ctx.game, &gst);
            game_engine_get_config(s_ctx.game, &gcfg);
            apply_game_outputs_to_status(&st, &gst, &gcfg);
        }
        int len = snprintf(msg, sizeof(msg),
                           "{\"ts\":%lld,\"pressure_kpa\":%.3f,\"temp_c\":%.2f,"
                           "\"sensor_status\":%u,\"dac\":{\"code\":%u,\"pd_mode\":%u,\"voltage\":%.3f},"
                           "\"pwm\":[%u,%u,%u,%u],\"ble\":{\"swing\":%u,\"vibrate\":%u},"
                           "\"game\":{\"running\":%s,\"paused\":%s,\"state\":\"%s\","
                           "\"currentPressure\":%.3f,\"averagePressure\":%.3f,\"currentIntensity\":%.2f,"
                           "\"targetIntensity\":%.2f,\"midPressure\":%.3f,\"criticalPressure\":%.3f,"
                           "\"midIntensity\":%.2f,\"edgingCount\":%" PRIu32 ",\"shockCount\":%" PRIu32 ","
                           "\"totalStimulationTime\":%.1f,\"isShocking\":%s}}",
                           (long long)st.timestamp_ms,
                           st.pressure_kpa,
                           st.temp_c,
                           st.sensor_status,
                           st.dac_code,
                           st.dac_pd,
                           st.dac_voltage,
                           st.pwm_permille[0], st.pwm_permille[1], st.pwm_permille[2], st.pwm_permille[3],
                           st.ble_swing,
                           st.ble_vibrate,
                           gst.running ? "true" : "false",
                           gst.paused ? "true" : "false",
                           game_state_to_string(gst.state),
                           gst.current_pressure,
                           gst.average_pressure,
                           gst.current_intensity,
                           gst.target_intensity,
                           gcfg.mid_pressure_kpa,
                           gcfg.critical_pressure_kpa,
                           gst.mid_intensity,
                           gst.edging_count,
                           gst.shock_count,
                           gst.total_stimulation_time_s,
                           gst.is_shocking ? "true" : "false");
        if (len > 0 && len < (int)sizeof(msg)) {
            ws_broadcast(msg);
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / ws_hz));
    }
}

esp_err_t web_server_start(const web_server_ctx_t *ctx)
{
    if (!ctx || !ctx->control || !ctx->telemetry || !ctx->game) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ctx = *ctx;
    ws_clients_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 16;
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

    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &config_get_uri);
    httpd_register_uri_handler(s_server, &config_post_uri);
    httpd_register_uri_handler(s_server, &game_status_uri);
    httpd_register_uri_handler(s_server, &game_config_get_uri);
    httpd_register_uri_handler(s_server, &game_config_post_uri);
    httpd_register_uri_handler(s_server, &game_control_post_uri);
    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &app_js_uri);
    httpd_register_uri_handler(s_server, &app_css_uri);

    if (!s_ws_task) {
        xTaskCreate(ws_push_task, "ws_push", 4096, NULL, 5, &s_ws_task);
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
