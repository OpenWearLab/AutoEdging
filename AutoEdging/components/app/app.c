#include "app.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "mdns.h"

#include "bus_i2c.h"
#include "dev_mcp_h11.h"
#include "dev_dac7571.h"
#include "act_pwm_ledc.h"
#include "ble_belt.h"

#include "control_api.h"
#include "telemetry.h"
#include "web_server.h"

static const char *TAG = "app";

#define APP_I2C_PORT        I2C_NUM_0
#define APP_I2C_SDA_GPIO    GPIO_NUM_5
#define APP_I2C_SCL_GPIO    GPIO_NUM_4
#define APP_I2C_FREQ_HZ     400000

#define M0_GPIO             GPIO_NUM_10
#define M1_GPIO             GPIO_NUM_11
#define M2_GPIO             GPIO_NUM_12
#define M3_GPIO             GPIO_NUM_13

#define TELEMETRY_MAX_SEC   120
#define TELEMETRY_MAX_HZ    50
#define TELEMETRY_MAX_POINTS (TELEMETRY_MAX_SEC * TELEMETRY_MAX_HZ)

static bus_i2c_t s_i2c = {0};
static mcp_h11_t s_mcp = {0};
static dac7571_t s_dac = {0};
static pwm_ledc_t s_pwm = {0};
static control_service_t s_service;
static telemetry_t s_telemetry;
static telemetry_point_t s_telemetry_buf[TELEMETRY_MAX_POINTS];
static SemaphoreHandle_t s_i2c_mutex = NULL;

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static esp_err_t nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t app_init_devices(void)
{
    bus_i2c_config_t bus_cfg = {
        .port = APP_I2C_PORT,
        .sda_io = APP_I2C_SDA_GPIO,
        .scl_io = APP_I2C_SCL_GPIO,
        .clk_speed_hz = APP_I2C_FREQ_HZ,
        .enable_internal_pullups = true,
        .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(bus_i2c_init(&s_i2c, &bus_cfg), TAG, "i2c init failed");

    mcp_h11_config_t mcp_cfg = {
        .i2c_addr_7bit = 0x36,
        .scl_speed_hz = APP_I2C_FREQ_HZ,
        .a = 50.0f,
        .b = -5.0f,
    };
    ESP_RETURN_ON_ERROR(mcp_h11_init(&s_mcp, s_i2c.bus, &mcp_cfg), TAG, "mcp init failed");

    dac7571_config_t dac_cfg = {
        .i2c_addr_7bit = 0x4D,
        .scl_speed_hz = APP_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(dac7571_init(&s_dac, s_i2c.bus, &dac_cfg), TAG, "dac init failed");

    pwm_ledc_config_t pwm_cfg = {
        .timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer = LEDC_TIMER_0,
            .pwm_freq_hz = 20000,
            .clk_cfg = LEDC_AUTO_CLK,
            .src_clk_hz = 80000000,
            .duty_resolution = 0,
        },
        .ch_cfg = {
            { .motor_id=0, .gpio=M0_GPIO, .channel=LEDC_CHANNEL_0, .init_duty_permille=0, .output_invert=false },
            { .motor_id=1, .gpio=M1_GPIO, .channel=LEDC_CHANNEL_1, .init_duty_permille=0, .output_invert=false },
            { .motor_id=2, .gpio=M2_GPIO, .channel=LEDC_CHANNEL_2, .init_duty_permille=0, .output_invert=false },
            { .motor_id=3, .gpio=M3_GPIO, .channel=LEDC_CHANNEL_3, .init_duty_permille=0, .output_invert=false },
        },
    };
    ESP_RETURN_ON_ERROR(pwm_ledc_init(&s_pwm, &pwm_cfg), TAG, "pwm init failed");

    ESP_RETURN_ON_ERROR(ble_belt_init(), TAG, "ble init failed");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < CONFIG_APP_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry++;
            ESP_LOGW(TAG, "retrying wifi (%d)", s_wifi_retry);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL), TAG, "wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL), TAG, "ip handler failed");

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_APP_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", CONFIG_APP_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (strlen(CONFIG_APP_WIFI_PASSWORD) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "wifi init done, connecting to %s", CONFIG_APP_WIFI_SSID);
    return ESP_OK;
}

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%u, used=%u", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

static esp_err_t mdns_init_app(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_APP_MDNS_HOSTNAME), TAG, "mdns hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("AutoEdging"), TAG, "mdns instance failed");
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0), TAG, "mdns service failed");
    ESP_LOGI(TAG, "mDNS hostname: %s.local", CONFIG_APP_MDNS_HOSTNAME);
    return ESP_OK;
}

static void sensor_task(void *arg)
{
    (void)arg;
    int64_t last_log_ms = 0;

    while (1) {
        control_config_t cfg = {0};
        control_service_get_config(&s_service, &cfg);
        uint32_t hz = cfg.sample_hz;
        if (hz < 1) {
            hz = 1;
        }
        if (hz > TELEMETRY_MAX_HZ) {
            hz = TELEMETRY_MAX_HZ;
        }

        TickType_t delay_ticks = pdMS_TO_TICKS(1000 / hz);

        mcp_h11_sample_t sample = {0};
        if (s_i2c_mutex) {
            xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);
        }
        esp_err_t err = mcp_h11_read_sample(&s_mcp, &sample);
        if (s_i2c_mutex) {
            xSemaphoreGive(s_i2c_mutex);
        }

        int64_t ts_ms = esp_timer_get_time() / 1000;
        if (err == ESP_OK) {
            control_service_update_sensor(&s_service, sample.pressure_kpa, sample.temp_c, sample.status, ts_ms);
            telemetry_point_t tp = {
                .ts_ms = ts_ms,
                .pressure_kpa = sample.pressure_kpa,
                .temp_c = sample.temp_c,
            };
            telemetry_push(&s_telemetry, &tp);
        } else {
            if ((ts_ms - last_log_ms) > 1000) {
                ESP_LOGW(TAG, "mcp read failed: %s", esp_err_to_name(err));
                last_log_ms = ts_ms;
            }
        }

        vTaskDelay(delay_ticks);
    }
}

void app_start(void)
{
    ESP_LOGI(TAG, "booting...");
    ESP_ERROR_CHECK(nvs_init());

    s_i2c_mutex = xSemaphoreCreateMutex();
    if (!s_i2c_mutex) {
        ESP_LOGE(TAG, "i2c mutex alloc failed");
        return;
    }

    ESP_ERROR_CHECK(app_init_devices());

    control_config_t cfg = {0};
    esp_err_t err = control_config_load(&cfg);
    if (err != ESP_OK || control_config_validate(&cfg, NULL, 0) != ESP_OK) {
        control_config_set_defaults(&cfg);
        control_config_save(&cfg);
    }

    control_service_hw_t hw = {
        .dac = &s_dac,
        .pwm = &s_pwm,
        .i2c_mutex = s_i2c_mutex,
    };
    ESP_ERROR_CHECK(control_service_init(&s_service, &hw, &cfg));

    telemetry_init(&s_telemetry, s_telemetry_buf, TELEMETRY_MAX_POINTS);

    ESP_ERROR_CHECK(wifi_init_sta());
    ESP_ERROR_CHECK(mdns_init_app());
    ESP_ERROR_CHECK(spiffs_init());

    web_server_ctx_t ws_ctx = {
        .control = &s_service,
        .telemetry = &s_telemetry,
    };
    ESP_ERROR_CHECK(web_server_start(&ws_ctx));

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "app started");
}
