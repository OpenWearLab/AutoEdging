#include "app.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "led_strip.h"

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
#include "game_engine.h"

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
#define STATUS_LED_GPIO     GPIO_NUM_38

#define TELEMETRY_MAX_SEC   120
#define TELEMETRY_MAX_HZ    100
#define TELEMETRY_MAX_POINTS (TELEMETRY_MAX_SEC * TELEMETRY_MAX_HZ)

static bus_i2c_t s_i2c = {0};
static mcp_h11_t s_mcp = {0};
static dac7571_t s_dac = {0};
static pwm_ledc_t s_pwm = {0};
static control_service_t s_service;
static telemetry_t s_telemetry;
static telemetry_point_t *s_telemetry_buf = NULL;
static SemaphoreHandle_t s_i2c_mutex = NULL;
static game_engine_t s_game = {0};
static led_strip_handle_t s_led = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_failed = false;

static esp_err_t status_led_init(void);
static void status_led_set(uint8_t r, uint8_t g, uint8_t b);
#if CONFIG_APP_MEMORY_LOG_ENABLE
static void memory_log_task(void *arg);
#endif

typedef enum {
    LED_MODE_WIFI_WAIT = 0,
    LED_MODE_WIFI_FAIL,
    LED_MODE_IDLE,
    LED_MODE_PAUSED,
    LED_MODE_CALM,
    LED_MODE_MIDDLE,
    LED_MODE_EDGING,
    LED_MODE_DELAY,
} led_mode_t;

static led_mode_t led_mode_from_status(void)
{
    if (s_wifi_failed) {
        return LED_MODE_WIFI_FAIL;
    }
    if (!s_wifi_connected) {
        return LED_MODE_WIFI_WAIT;
    }

    game_status_t st = {0};
    game_engine_get_status(&s_game, &st);
    if (!st.running) {
        return LED_MODE_IDLE;
    }
    if (st.paused) {
        return LED_MODE_PAUSED;
    }
    switch (st.state) {
    case GAME_STATE_INITIAL_CALM:
    case GAME_STATE_SUB_CALM:
        return LED_MODE_CALM;
    case GAME_STATE_MIDDLE:
        return LED_MODE_MIDDLE;
    case GAME_STATE_EDGING:
        return LED_MODE_EDGING;
    case GAME_STATE_DELAY:
        return LED_MODE_DELAY;
    default:
        return LED_MODE_IDLE;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    led_mode_t mode = LED_MODE_WIFI_WAIT;
    bool on = true;
    int64_t last_toggle_ms = 0;

    while (1) {
        control_config_t cfg = {0};
        control_service_get_config(&s_service, &cfg);
        if (!cfg.status_led_enabled) {
            status_led_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        led_mode_t next = led_mode_from_status();
        if (next != mode) {
            mode = next;
            on = true;
            last_toggle_ms = 0;
        }

        uint8_t r = 0, g = 0, b = 0;
        int period_ms = 0;

        switch (mode) {
        case LED_MODE_WIFI_WAIT:
            r = 255; g = 180; b = 0; period_ms = 0; // yellow slow blink
            break;
        case LED_MODE_WIFI_FAIL:
            r = 255; g = 0; b = 0; period_ms = 0; // red steady
            break;
        case LED_MODE_IDLE:
            r = 0; g = 120; b = 255; period_ms = 0; // green steady
            break;
        case LED_MODE_PAUSED:
            r = 0; g = 120; b = 255; period_ms = 600; // blue slow blink
            break;
        case LED_MODE_CALM:
            r = 0; g = 255; b = 0; period_ms = 600; // green slow blink
            break;
        case LED_MODE_MIDDLE:
            r = 255; g = 180; b = 0; period_ms = 250; // yellow fast blink
            break;
        case LED_MODE_EDGING:
            r = 255; g = 5; b = 0; period_ms = 200; // red fast blink
            break;
        case LED_MODE_DELAY:
            r = 180; g = 0; b = 255; period_ms = 250; // purple fast blink
            break;
        default:
            break;
        }

        if (period_ms > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (last_toggle_ms == 0 || (now_ms - last_toggle_ms) >= period_ms) {
                on = !on;
                last_toggle_ms = now_ms;
            }
            if (on) {
                status_led_set(r, g, b);
            } else {
                status_led_set(0, 0, 0);
            }
        } else {
            status_led_set(r, g, b);
        }

        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static esp_err_t status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led);
    if (err == ESP_OK && s_led) {
        led_strip_clear(s_led);
    }
    return err;
}

static void status_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) {
        return;
    }
    led_strip_set_pixel(s_led, 0, g, r, b);
    led_strip_refresh(s_led);
}

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
        s_wifi_connected = false;
        s_wifi_failed = false;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < CONFIG_APP_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry++;
            ESP_LOGW(TAG, "retrying wifi (%d)", s_wifi_retry);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            s_wifi_failed = true;
            s_wifi_connected = false;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_wifi_connected = true;
        s_wifi_failed = false;
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
        if (err == ESP_OK && sample.status != 0x00) {
            control_service_update_sensor(&s_service, sample.pressure_kpa, sample.temp_c, sample.status, ts_ms);
            telemetry_point_t tp = {
                .ts_ms = ts_ms,
                .pressure_kpa = sample.pressure_kpa,
                .temp_c = sample.temp_c,
            };
            telemetry_push(&s_telemetry, &tp);
#ifdef CONFIG_APP_DEBUG_IO
            ESP_LOGI(TAG, "io: mcp_h11 p=%.3f kPa t=%.2f C status=0x%02X",
                     (double)sample.pressure_kpa, (double)sample.temp_c, sample.status);
#endif
            game_engine_on_sample(&s_game, sample.pressure_kpa, ts_ms);
        } else {
            if ((ts_ms - last_log_ms) > 1000) {
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "mcp read failed: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGW(TAG, "mcp sample dropped: invalid status=0x%02X", sample.status);
                }
                last_log_ms = ts_ms;
            }
        }

        vTaskDelay(delay_ticks);
    }
}

#if CONFIG_APP_MEMORY_LOG_ENABLE
static void memory_log_task(void *arg)
{
    (void)arg;

    const TickType_t delay_ticks = pdMS_TO_TICKS(10000);

    while (1) {
        size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

        size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (psram_total > 0) {
            ESP_LOGI(TAG,
                     "mem internal used=%u free=%u min=%u largest=%u total=%u | psram used=%u free=%u min=%u largest=%u total=%u",
                     (unsigned)(internal_total - internal_free),
                     (unsigned)internal_free,
                     (unsigned)internal_min_free,
                     (unsigned)internal_largest,
                     (unsigned)internal_total,
                     (unsigned)(psram_total - psram_free),
                     (unsigned)psram_free,
                     (unsigned)psram_min_free,
                     (unsigned)psram_largest,
                     (unsigned)psram_total);
        } else {
            ESP_LOGI(TAG,
                     "mem internal used=%u free=%u min=%u largest=%u total=%u | psram unavailable",
                     (unsigned)(internal_total - internal_free),
                     (unsigned)internal_free,
                     (unsigned)internal_min_free,
                     (unsigned)internal_largest,
                     (unsigned)internal_total);
        }

        vTaskDelay(delay_ticks);
    }
}
#endif

void app_start(void)
{
    ESP_LOGI(TAG, "booting...");
    ESP_ERROR_CHECK(nvs_init());
    if (status_led_init() == ESP_OK) {
        status_led_set(255, 180, 0);
    } else {
        ESP_LOGW(TAG, "status led init failed");
    }

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
    cfg.dac_code = 0;
    cfg.dac_pd = DAC7571_PD_NORMAL;
    cfg.ble_swing = 0;
    cfg.ble_vibrate = 0;
    for (int i = 0; i < 4; i++) {
        cfg.pwm_permille[i] = 0;
    }

    control_service_hw_t hw = {
        .dac = &s_dac,
        .pwm = &s_pwm,
        .i2c_mutex = s_i2c_mutex,
    };
    ESP_ERROR_CHECK(control_service_init(&s_service, &hw, &cfg));

    game_config_t game_cfg = {0};
    err = game_config_load(&game_cfg);
    if (err != ESP_OK || game_config_validate(&game_cfg, NULL, 0) != ESP_OK) {
        game_config_set_defaults(&game_cfg);
        game_config_save(&game_cfg);
    }
    game_engine_hw_t game_hw = {
        .dac = &s_dac,
        .pwm = &s_pwm,
        .i2c_mutex = s_i2c_mutex,
        .led = s_led,
    };
    ESP_ERROR_CHECK(game_engine_init(&s_game, &game_hw, &game_cfg));

    // Allocate telemetry buffer from external memory (PSRAM) if available
    s_telemetry_buf = heap_caps_malloc(TELEMETRY_MAX_POINTS * sizeof(telemetry_point_t), 
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_telemetry_buf == NULL) {
        // Fallback to internal memory if PSRAM not available
        s_telemetry_buf = malloc(TELEMETRY_MAX_POINTS * sizeof(telemetry_point_t));
        ESP_ERROR_CHECK(s_telemetry_buf ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_LOGI(TAG, "Telemetry buffer allocated in internal memory: %zu bytes", 
                 TELEMETRY_MAX_POINTS * sizeof(telemetry_point_t));
    } else {
        ESP_LOGI(TAG, "Telemetry buffer allocated in PSRAM: %zu bytes", 
                 TELEMETRY_MAX_POINTS * sizeof(telemetry_point_t));
    }

    telemetry_init(&s_telemetry, s_telemetry_buf, TELEMETRY_MAX_POINTS);

    ESP_ERROR_CHECK(wifi_init_sta());
    ESP_ERROR_CHECK(mdns_init_app());
    ESP_ERROR_CHECK(spiffs_init());

    web_server_ctx_t ws_ctx = {
        .control = &s_service,
        .telemetry = &s_telemetry,
        .game = &s_game,
    };
    ESP_ERROR_CHECK(web_server_start(&ws_ctx));

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(led_task, "led_task", 2048, NULL, 4, NULL);
#if CONFIG_APP_MEMORY_LOG_ENABLE
    xTaskCreate(memory_log_task, "memory_log_task", 3072, NULL, 1, NULL);
#endif

    ESP_LOGI(TAG, "app started");
}
