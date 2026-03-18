#include "wifi_service.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "wifi_service";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_STA_FAIL_BIT  BIT1
#define WIFI_PROV_END_BIT  BIT2

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;
static bool s_stack_inited;
static bool s_prov_mgr_inited;
static int s_sta_retry;
static wifi_service_status_t s_status;

static void wifi_service_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void wifi_service_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void wifi_service_set_connection_state(bool connected, bool failed)
{
    wifi_service_lock();
    s_status.connected = connected;
    s_status.connect_failed = failed;
    if (!connected) {
        s_status.ip_addr[0] = '\0';
    }
    wifi_service_unlock();
}

static void wifi_service_set_provisioning(bool provisioning)
{
    wifi_service_lock();
    s_status.provisioning = provisioning;
    if (provisioning) {
        s_status.connect_failed = false;
        s_status.connected = false;
        s_status.ip_addr[0] = '\0';
    }
    wifi_service_unlock();
}

static void wifi_service_set_provisioned(bool provisioned)
{
    wifi_service_lock();
    s_status.provisioned = provisioned;
    if (!provisioned) {
        s_status.ssid[0] = '\0';
    }
    wifi_service_unlock();
}

static void wifi_service_set_ip(const esp_ip4_addr_t *ip)
{
    wifi_service_lock();
    if (ip) {
        snprintf(s_status.ip_addr, sizeof(s_status.ip_addr), IPSTR, IP2STR(ip));
    } else {
        s_status.ip_addr[0] = '\0';
    }
    wifi_service_unlock();
}

static void wifi_service_set_ssid_from_sta_cfg(const wifi_sta_config_t *sta)
{
    wifi_service_lock();
    if (sta && sta->ssid[0] != '\0') {
        snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", (const char *)sta->ssid);
    } else {
        s_status.ssid[0] = '\0';
    }
    wifi_service_unlock();
}

static void wifi_service_load_ssid_from_wifi(void)
{
    wifi_config_t cfg = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        wifi_service_set_ssid_from_sta_cfg(&cfg.sta);
    }
}

static void wifi_service_build_service_name(void)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));

    wifi_service_lock();
    snprintf(s_status.service_name, sizeof(s_status.service_name), "%s%02X%02X%02X",
             CONFIG_APP_WIFI_PROV_SERVICE_PREFIX, mac[3], mac[4], mac[5]);
    wifi_service_unlock();
}

static void wifi_service_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_sta_retry = 0;

            wifi_service_lock();
            bool provisioning = s_status.provisioning;
            wifi_service_unlock();

            if (!provisioning) {
                wifi_service_set_connection_state(false, false);
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_service_lock();
            bool provisioning = s_status.provisioning;
            bool reboot_pending = s_status.reboot_pending;
            wifi_service_unlock();

            wifi_service_set_connection_state(false, false);

            if (reboot_pending) {
                return;
            }
            if (provisioning) {
                return;
            }

            if (s_sta_retry < CONFIG_APP_WIFI_MAX_RETRY) {
                esp_wifi_connect();
                s_sta_retry++;
                ESP_LOGW(TAG, "retrying saved Wi-Fi (%d/%d)", s_sta_retry, CONFIG_APP_WIFI_MAX_RETRY);
            } else {
                wifi_service_set_connection_state(false, true);
                xEventGroupSetBits(s_events, WIFI_STA_FAIL_BIT);
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_retry = 0;
        wifi_service_set_connection_state(true, false);
        wifi_service_set_ip(&event->ip_info.ip);
        wifi_service_load_ssid_from_wifi();
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connected, got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        return;
    }

    if (event_base != WIFI_PROV_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_PROV_START:
        wifi_service_set_provisioning(true);
        ESP_LOGI(TAG, "BLE provisioning started");
        break;
    case WIFI_PROV_CRED_RECV: {
        wifi_sta_config_t *sta = (wifi_sta_config_t *)event_data;
        wifi_service_set_ssid_from_sta_cfg(sta);
        ESP_LOGI(TAG, "received Wi-Fi credentials for SSID: %s", sta ? (const char *)sta->ssid : "(null)");
        break;
    }
    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
        ESP_LOGW(TAG, "provisioning failed: %s, waiting for new credentials",
                 (reason && *reason == WIFI_PROV_STA_AUTH_ERROR) ? "auth error" : "ap not found");
        esp_err_t err = wifi_prov_mgr_reset_sm_state_on_failure();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reset provisioning state machine failed: %s", esp_err_to_name(err));
        }
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        wifi_service_set_provisioned(true);
        ESP_LOGI(TAG, "provisioning successful");
        break;
    case WIFI_PROV_END:
        wifi_service_set_provisioning(false);
        if (s_prov_mgr_inited) {
            wifi_prov_mgr_deinit();
            s_prov_mgr_inited = false;
        }
        xEventGroupSetBits(s_events, WIFI_PROV_END_BIT);
        ESP_LOGI(TAG, "provisioning ended");
        break;
    default:
        break;
    }
}

static esp_err_t wifi_service_init_stack(void)
{
    if (s_stack_inited) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set initial wifi mode failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_service_event_handler, NULL),
                        TAG, "register wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &wifi_service_event_handler, NULL),
                        TAG, "register ip handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_service_event_handler, NULL),
                        TAG, "register prov handler failed");

    wifi_service_build_service_name();
    s_stack_inited = true;
    return ESP_OK;
}

static esp_err_t wifi_service_init_prov_mgr(void)
{
    if (s_prov_mgr_inited) {
        return ESP_OK;
    }

    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .wifi_prov_conn_cfg = {
            .wifi_conn_attempts = CONFIG_APP_WIFI_MAX_RETRY,
        },
    };

    ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(cfg), TAG, "provisioning manager init failed");
    s_prov_mgr_inited = true;
    return ESP_OK;
}

static void wifi_service_deinit_prov_mgr(void)
{
    if (!s_prov_mgr_inited) {
        return;
    }
    wifi_prov_mgr_deinit();
    s_prov_mgr_inited = false;
}

static esp_err_t wifi_service_probe_saved_credentials(bool *provisioned)
{
    ESP_RETURN_ON_ERROR(wifi_service_init_prov_mgr(), TAG, "provisioning manager init failed");
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_is_provisioned(provisioned), TAG, "probe provisioned state failed");
    wifi_service_set_provisioned(*provisioned);
    if (*provisioned) {
        wifi_service_load_ssid_from_wifi();
    }
    wifi_service_deinit_prov_mgr();
    return ESP_OK;
}

static esp_err_t wifi_service_stop_wifi(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t wifi_service_clear_credentials(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_restore(), TAG, "wifi restore failed");
    wifi_service_lock();
    s_status.provisioned = false;
    s_status.connected = false;
    s_status.connect_failed = false;
    s_status.ssid[0] = '\0';
    s_status.ip_addr[0] = '\0';
    wifi_service_unlock();
    return ESP_OK;
}

static esp_err_t wifi_service_connect_saved(void)
{
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_STA_FAIL_BIT);
    wifi_service_set_provisioning(false);
    wifi_service_set_connection_state(false, false);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_CONNECTED_BIT | WIFI_STA_FAIL_BIT,
                                           pdTRUE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t wifi_service_wait_for_prov_end(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_PROV_END_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    if (bits & WIFI_PROV_END_BIT) {
        return ESP_OK;
    }

    if (!s_prov_mgr_inited) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "forcing provisioning stop after success");
    wifi_prov_mgr_stop_provisioning();

    bits = xEventGroupWaitBits(s_events, WIFI_PROV_END_BIT,
                               pdTRUE, pdFALSE, pdMS_TO_TICKS(4000));
    if (bits & WIFI_PROV_END_BIT) {
        return ESP_OK;
    }

    wifi_service_deinit_prov_mgr();
    return ESP_OK;
}

static esp_err_t wifi_service_start_provisioning(bool reset_existing_credentials)
{
    char service_name[32] = {0};

    if (reset_existing_credentials) {
        ESP_RETURN_ON_ERROR(wifi_service_clear_credentials(), TAG, "clear credentials failed");
    }
    ESP_RETURN_ON_ERROR(wifi_service_stop_wifi(), TAG, "stop wifi before provisioning failed");
    ESP_RETURN_ON_ERROR(wifi_service_init_prov_mgr(), TAG, "provisioning manager init failed");

    if (reset_existing_credentials) {
        ESP_RETURN_ON_ERROR(wifi_prov_mgr_reset_provisioning(), TAG, "reset provisioning failed");
    }

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_STA_FAIL_BIT | WIFI_PROV_END_BIT);
    wifi_service_set_provisioning(true);
    wifi_service_lock();
    snprintf(service_name, sizeof(service_name), "%s", s_status.service_name);
    wifi_service_unlock();

    ESP_LOGI(TAG, "starting BLE provisioning: device=%s, pop=%s",
             service_name, CONFIG_APP_WIFI_PROV_POP);

    esp_err_t err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                                     CONFIG_APP_WIFI_PROV_POP,
                                                     service_name,
                                                     NULL);
    if (err != ESP_OK) {
        wifi_service_set_provisioning(false);
        wifi_service_deinit_prov_mgr();
        return err;
    }

    xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
    return wifi_service_wait_for_prov_end();
}

static void wifi_service_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1200));
    (void)esp_wifi_disconnect();
    (void)wifi_service_clear_credentials();
    esp_restart();
}

esp_err_t wifi_service_start(void)
{
    bool provisioned = false;

    ESP_RETURN_ON_ERROR(wifi_service_init_stack(), TAG, "wifi stack init failed");
    ESP_RETURN_ON_ERROR(wifi_service_probe_saved_credentials(&provisioned), TAG, "probe saved credentials failed");

    if (provisioned) {
        ESP_LOGI(TAG, "saved Wi-Fi credentials found, trying SSID: %s",
                 s_status.ssid[0] ? s_status.ssid : "(hidden)");
        if (wifi_service_connect_saved() == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "saved Wi-Fi connection failed, falling back to BLE provisioning");
    } else {
        ESP_LOGI(TAG, "no saved Wi-Fi credentials, entering BLE provisioning");
    }

    return wifi_service_start_provisioning(provisioned);
}

void wifi_service_get_status(wifi_service_status_t *out)
{
    if (!out) {
        return;
    }
    wifi_service_lock();
    *out = s_status;
    wifi_service_unlock();
}

bool wifi_service_is_connected(void)
{
    bool connected = false;
    wifi_service_lock();
    connected = s_status.connected;
    wifi_service_unlock();
    return connected;
}

bool wifi_service_has_failed(void)
{
    bool failed = false;
    wifi_service_lock();
    failed = s_status.connect_failed;
    wifi_service_unlock();
    return failed;
}

esp_err_t wifi_service_request_reprovision_reboot(void)
{
    wifi_service_lock();
    bool reboot_pending = s_status.reboot_pending;
    wifi_service_unlock();
    if (reboot_pending) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_service_lock();
    s_status.reboot_pending = true;
    s_status.connected = false;
    s_status.connect_failed = false;
    s_status.ip_addr[0] = '\0';
    wifi_service_unlock();

    BaseType_t ok = xTaskCreate(wifi_service_reboot_task, "wifi_reboot", 2048, NULL, 4, NULL);
    if (ok == pdPASS) {
        return ESP_OK;
    }

    wifi_service_lock();
    s_status.reboot_pending = false;
    wifi_service_unlock();
    return ESP_ERR_NO_MEM;
}
