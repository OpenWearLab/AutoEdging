#include "web_server.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"

static const char *TAG = "web_server";

#define WS_MAX_CLIENTS 4
#define MAX_POST_BODY  2048

static httpd_handle_t s_server = NULL;
static web_server_ctx_t s_ctx = {0};
static int s_ws_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_lock = NULL;
static TaskHandle_t s_ws_task = NULL;

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

static esp_err_t handle_get_status(httpd_req_t *req)
{
    control_status_t st = {0};
    control_service_get_status(s_ctx.control, &st);

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
    char msg[256];

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
        int len = snprintf(msg, sizeof(msg),
                           "{\"ts\":%lld,\"pressure_kpa\":%.3f,\"temp_c\":%.2f,"
                           "\"sensor_status\":%u,\"dac\":{\"code\":%u,\"pd_mode\":%u,\"voltage\":%.3f},"
                           "\"pwm\":[%u,%u,%u,%u],\"ble\":{\"swing\":%u,\"vibrate\":%u}}",
                           (long long)st.timestamp_ms,
                           st.pressure_kpa,
                           st.temp_c,
                           st.sensor_status,
                           st.dac_code,
                           st.dac_pd,
                           st.dac_voltage,
                           st.pwm_permille[0], st.pwm_permille[1], st.pwm_permille[2], st.pwm_permille[3],
                           st.ble_swing,
                           st.ble_vibrate);
        if (len > 0 && len < (int)sizeof(msg)) {
            ws_broadcast(msg);
        }

        vTaskDelay(pdMS_TO_TICKS(1000 / ws_hz));
    }
}

esp_err_t web_server_start(const web_server_ctx_t *ctx)
{
    if (!ctx || !ctx->control || !ctx->telemetry) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ctx = *ctx;
    ws_clients_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
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
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws,
        .is_websocket = true,
    };
    httpd_uri_t ws_uri_post = {
        .uri = "/ws",
        .method = HTTP_POST,
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
    httpd_register_uri_handler(s_server, &ws_uri);
    httpd_register_uri_handler(s_server, &ws_uri_post);
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
