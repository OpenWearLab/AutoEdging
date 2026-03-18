#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

static const char *TAG = "ble_belt";

typedef struct {
    const uint8_t *data;
    uint16_t len;
} adv_payload_t;

#define ADV_PL(...) \
    { .data = (const uint8_t[]){ __VA_ARGS__ }, .len = sizeof((const uint8_t[]){ __VA_ARGS__ }) }

/* ====== 写死 payload（与你测试代码一致）====== */
static const adv_payload_t g_vibrate_payloads[10] = {
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD5,0x96,0x4C, 0x03,0x03,0x8F,0xAE), /* Stop vibrate */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD4,0x1F,0x5D, 0x03,0x03,0x8F,0xAE), /* 低强度震动 */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD7,0x84,0x6F, 0x03,0x03,0x8F,0xAE), /* 中强度震动 */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD6,0x0D,0x7E, 0x03,0x03,0x8F,0xAE), /* 高强度震动 */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD1,0xB2,0x0A, 0x03,0x03,0x8F,0xAE), /* 欢乐恰恰（启停循环） */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD0,0x3B,0x1B, 0x03,0x03,0x8F,0xAE), /* 海中冲浪（） */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD3,0xA0,0x29, 0x03,0x03,0x8F,0xAE), /* 激情探戈（） */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xD2,0x29,0x38, 0x03,0x03,0x8F,0xAE), /* 小鹿乱撞（） */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xDD,0xDE,0xC0, 0x03,0x03,0x8F,0xAE), /* 大浪拍岸（） */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xDC,0x57,0xD1, 0x03,0x03,0x8F,0xAE), /* 潮起潮落（） */
};

static const adv_payload_t g_swing_payloads[10] = {
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA5,0x11,0x3F, 0x03,0x03,0x8F,0xAE), /* Stop Swing */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA4,0x98,0x2E, 0x03,0x03,0x8F,0xAE), /* 低速摆动 */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA7,0x03,0x1C, 0x03,0x03,0x8F,0xAE), /* 中速摆动 */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA6,0x8A,0x0D, 0x03,0x03,0x8F,0xAE), /* 高速摆动 */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA1,0x35,0x79, 0x03,0x03,0x8F,0xAE), /* 欢乐恰恰（启停循环） */
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA0,0xBC,0x68, 0x03,0x03,0x8F,0xAE),
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA3,0x27,0x5A, 0x03,0x03,0x8F,0xAE),
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xA2,0xAE,0x4B, 0x03,0x03,0x8F,0xAE),
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xAD,0x59,0xB3, 0x03,0x03,0x8F,0xAE),
    ADV_PL(0x02,0x01,0x01, 0x0E,0xFF,0xFF,0x00, 0x6D,0xB6,0x43,0xCE,0x97,0xFE,0x42,0x7C,0xAC,0xD0,0xA2, 0x03,0x03,0x8F,0xAE),
};

// ===== 新增：空闲停止窗口（你可调，比如 50~300ms）=====
#ifndef BLE_BELT_IDLE_STOP_MS
#define BLE_BELT_IDLE_STOP_MS 50
#endif

static bool g_adv_running = false;

/* ====== 异步框架：队列 + 事件组 ====== */
typedef enum { CMD_VIBRATE, CMD_SWING } cmd_type_t;
typedef struct { cmd_type_t type; uint8_t idx; } ble_cmd_t;

static QueueHandle_t g_q;
static EventGroupHandle_t g_evt;
static TaskHandle_t g_task;

#define EVT_RAND_OK      (1U << 0)
#define EVT_DATA_OK      (1U << 1)
#define EVT_START_OK     (1U << 2)
#define EVT_STOP_OK      (1U << 3)
#define EVT_FAIL         (1U << 15)

static bool g_inited = false;

static uint8_t g_rand_addr[6] = {0xC2,0x11,0x22,0x33,0x44,0x55};

static esp_ble_adv_params_t g_adv_params = {
    .adv_int_min = 0x00A0,
    .adv_int_max = 0x00A0,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .peer_addr = {0},
    .peer_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        if (param->set_rand_addr_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            xEventGroupSetBits(g_evt, EVT_RAND_OK);
        } else {
            xEventGroupSetBits(g_evt, EVT_FAIL);
        }
        break;

    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            xEventGroupSetBits(g_evt, EVT_DATA_OK);
        } else {
            xEventGroupSetBits(g_evt, EVT_FAIL);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            xEventGroupSetBits(g_evt, EVT_START_OK);
        } else {
            xEventGroupSetBits(g_evt, EVT_FAIL);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            xEventGroupSetBits(g_evt, EVT_STOP_OK);
        } else {
            xEventGroupSetBits(g_evt, EVT_FAIL);
        }
        break;

    default:
        break;
    }
}

static esp_err_t stop_adv(void)
{
    return esp_ble_gap_stop_advertising();
}

static esp_err_t start_adv(void)
{
    return esp_ble_gap_start_advertising(&g_adv_params);
}

static esp_err_t config_payload(const adv_payload_t *pl)
{
    return esp_ble_gap_config_adv_data_raw((uint8_t *)pl->data, pl->len);
}

static const adv_payload_t *pick_payload(const ble_cmd_t *cmd)
{
    if (cmd->type == CMD_VIBRATE) return &g_vibrate_payloads[cmd->idx];
    return &g_swing_payloads[cmd->idx];
}

static esp_err_t wait_bits(EventBits_t bits, TickType_t to)
{
    EventBits_t got = xEventGroupWaitBits(g_evt, bits | EVT_FAIL, pdTRUE, pdTRUE, to);
    if (got & EVT_FAIL) return ESP_FAIL;
    if ((got & bits) != bits) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

static void ble_belt_task(void *arg)
{
    (void)arg;

    if (wait_bits(EVT_RAND_OK, pdMS_TO_TICKS(1500)) != ESP_OK) {
        ESP_LOGE(TAG, "BLE init handshake failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "BLE ready");
    g_adv_running = false;

    for (;;) {
        ble_cmd_t c;

        // 广播在跑：用“空闲超时”作为 stop 条件；没跑：一直等到有命令
        TickType_t to = g_adv_running ? pdMS_TO_TICKS(BLE_BELT_IDLE_STOP_MS) : portMAX_DELAY;

        if (xQueueReceive(g_q, &c, to) != pdTRUE) {
            // 超时：队列持续空闲 -> stop
            if (g_adv_running) {
                ESP_LOGD(TAG, "idle %dms -> stop adv", BLE_BELT_IDLE_STOP_MS);
                xEventGroupClearBits(g_evt, EVT_STOP_OK | EVT_FAIL);
                esp_err_t err = stop_adv();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "stop adv failed after idle: %s", esp_err_to_name(err));
                } else if (wait_bits(EVT_STOP_OK, pdMS_TO_TICKS(500)) != ESP_OK) {
                    ESP_LOGW(TAG, "wait STOP_COMPLETE failed after idle");
                }
                g_adv_running = false;
            }
            continue;
        }

        // 不做 drain：每条命令都必须执行（严格 FIFO）
        const adv_payload_t *pl = pick_payload(&c);
        if (!pl || !pl->data || pl->len == 0) {
            ESP_LOGW(TAG, "skip empty payload");
            continue;
        }

        if (g_adv_running) {
            xEventGroupClearBits(g_evt, EVT_STOP_OK | EVT_FAIL);
            esp_err_t err = stop_adv();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "stop adv failed: %s", esp_err_to_name(err));
                continue;
            }

            err = wait_bits(EVT_STOP_OK, pdMS_TO_TICKS(500));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "wait STOP_COMPLETE failed: %s", esp_err_to_name(err));
                continue;
            }
            g_adv_running = false;
        }

        xEventGroupClearBits(g_evt, EVT_DATA_OK | EVT_FAIL);

        esp_err_t err = config_payload(pl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "config payload failed: %s", esp_err_to_name(err));
            continue;
        }

        err = wait_bits(EVT_DATA_OK, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wait DATA_SET_COMPLETE failed: %s", esp_err_to_name(err));
            continue;
        }

        xEventGroupClearBits(g_evt, EVT_START_OK | EVT_FAIL);

        err = start_adv();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start adv failed: %s", esp_err_to_name(err));
            continue;
        }

        err = wait_bits(EVT_START_OK, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wait START_COMPLETE failed: %s", esp_err_to_name(err));
            continue;
        }
        g_adv_running = true;

        ESP_LOGI(TAG, "sent cmd=%d idx=%u len=%u (adv_running=1)",
                 (int)c.type, (unsigned)c.idx, (unsigned)pl->len);
    }
}


esp_err_t ble_belt_init(void)
{
    if (g_inited) return ESP_OK;
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    g_evt = xEventGroupCreate();
    g_q = xQueueCreate(8, sizeof(ble_cmd_t)); // 队列深度可按需求调
    if (!g_evt || !g_q) return ESP_ERR_NO_MEM;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));

    ESP_ERROR_CHECK(esp_ble_gap_set_rand_addr(g_rand_addr));

    // 启动 BLE 任务（后续所有 stop/config/start 都在该任务里执行）
    xTaskCreate(ble_belt_task, "ble_belt", 4096, NULL, 8, &g_task);

    g_inited = true;
    return ESP_OK;
}

static esp_err_t enqueue_cmd(cmd_type_t type, int idx)
{
    if (!g_inited) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= 10) return ESP_ERR_INVALID_ARG;

    ble_cmd_t c = { .type = type, .idx = (uint8_t)idx };

    // 0 tick：不阻塞；需要阻塞可把 0 改成 pdMS_TO_TICKS(x)
    BaseType_t ok = xQueueSend(g_q, &c, 0); // 队列 API :contentReference[oaicite:11]{index=11}
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ble_belt_send_vibrate(int idx)
{
    return enqueue_cmd(CMD_VIBRATE, idx);
}

esp_err_t ble_belt_send_swing(int idx)
{
    return enqueue_cmd(CMD_SWING, idx);
}
