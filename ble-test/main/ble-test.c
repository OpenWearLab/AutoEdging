#include <stdbool.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

static const char *TAG = "EXT_ADV_REPLAY";

static uint8_t g_adv_handle = 0x02;
static uint8_t g_rand_addr[6] = {0xC2, 0x11, 0x22, 0x33, 0x44, 0x55};

typedef struct {
    const uint8_t *data;
    uint16_t len;
} adv_payload_t;

#define ADV_PL(...) \
    { .data = (const uint8_t[]){ __VA_ARGS__ }, .len = sizeof((const uint8_t[]){ __VA_ARGS__ }) }

/* 震动 1-10 / 摆动 1-10 的两张表；示例填入 3 组，其余留空待补齐 */
static adv_payload_t g_vibrate_payloads[10] = {
    ADV_PL(
        /* Flags */                0x02, 0x01, 0x01,
        /* Manufacturer Specific */0x0E, 0xFF, 0xFF, 0x00,
        /* data */                 0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD4, 0x1F, 0x5D,
        /* UUID16 */               0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD7, 0x84, 0x6F,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD6, 0x0D, 0x7E,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL( // Stop vibrate
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD5, 0x96, 0x4C,
        0x03, 0x03, 0x8F, 0xAE), 
    { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 },
};

static adv_payload_t g_swing_payloads[10] = {
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xA4, 0x98, 0x2e,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xA7, 0x03, 0x1C,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xA6, 0x8A, 0x0D,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL( // Stop swing
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xA5, 0x11, 0x3F,
        0x03, 0x03, 0x8F, 0xAE),
    { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, { NULL, 0 },
};

typedef struct {
    const adv_payload_t *list;
    size_t count;
    const char *name;
} adv_group_t;

static const adv_group_t g_groups[] = {
    { g_vibrate_payloads, 10, "vibrate" },
    { g_swing_payloads,   10, "swing"   },
};

static int g_state = 0;              /* 当前“第几个有效 payload”，按两组顺序摊平 */
static size_t g_total_payloads = 0;  /* 有效 payload 总数（len>0） */
static bool g_scan_rsp_configured = false;
static volatile bool g_adv_data_busy = false;
static esp_timer_handle_t g_timer = NULL;

/* 扩展广播参数：这里用“较通用”的可连/可扫配置，你可以按抓包进一步微调 */
static esp_ble_gap_ext_adv_params_t g_ext_adv_params = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY | ESP_BLE_GAP_SET_EXT_ADV_PROP_CONNECTABLE | ESP_BLE_GAP_SET_EXT_ADV_PROP_SCANNABLE,
    .interval_min = 0x00A0,  // 100ms (0.625ms unit)
    .interval_max = 0x00A0,
    .channel_map = ADV_CHNL_ALL,
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    .tx_power = ESP_BLE_PWR_TYPE_DEFAULT,
    .primary_phy = ESP_BLE_GAP_PHY_1M,
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_1M,
    .sid = 0,
    .scan_req_notif = false,
};

static size_t count_valid_payloads(void)
{
    size_t count = 0;
    for (size_t g = 0; g < sizeof(g_groups) / sizeof(g_groups[0]); ++g) {
        for (size_t i = 0; i < g_groups[g].count; ++i) {
            if (g_groups[g].list[i].data != NULL && g_groups[g].list[i].len > 0) {
                ++count;
            }
        }
    }
    return count;
}

static const adv_payload_t *get_payload_by_index(int index, int *out_group, int *out_idx)
{
    for (size_t g = 0; g < sizeof(g_groups) / sizeof(g_groups[0]); ++g) {
        for (size_t i = 0; i < g_groups[g].count; ++i) {
            const adv_payload_t *p = &g_groups[g].list[i];
            if (p->data == NULL || p->len == 0) {
                continue;
            }
            if (index == 0) {
                if (out_group) *out_group = (int)g;
                if (out_idx) *out_idx = (int)i;
                return p;
            }
            --index;
        }
    }
    return NULL;
}

static esp_err_t set_adv_data_for_state(int state)
{
    if (g_adv_data_busy) {
        ESP_LOGW(TAG, "Skip config: previous adv data still in progress");
        return ESP_ERR_INVALID_STATE;
    }

    int group_idx = -1, payload_idx = -1;
    const adv_payload_t *entry = get_payload_by_index(state, &group_idx, &payload_idx);
    if (!entry) {
        ESP_LOGE(TAG, "Invalid adv state %d (no payload defined)", state);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Config ext adv data: handle=0x%02x, state=%d (%s[%d]), len=%u",
             g_adv_handle, state, g_groups[group_idx].name, payload_idx + 1, (unsigned)entry->len);

    g_adv_data_busy = true;

    /* raw 扩展广播数据 */
    /* API signature: (instance, length, data) */
    esp_err_t err = esp_ble_gap_config_ext_adv_data_raw(g_adv_handle, entry->len, entry->data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_ext_adv_data_raw failed: %s", esp_err_to_name(err));
        g_adv_data_busy = false;
        return err;
    }

    /* raw 扫描响应数据（你抓包是 length=0） */
    /* length = 0, data = NULL to express "no scan response" */
    if (!g_scan_rsp_configured) {
        err = esp_ble_gap_config_ext_scan_rsp_data_raw(g_adv_handle, 0, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ble_gap_config_ext_scan_rsp_data_raw failed: %s", esp_err_to_name(err));
            g_adv_data_busy = false;
            return err;
        }
        g_scan_rsp_configured = true;
    }

    return ESP_OK;
}

static void start_ext_adv(void)
{
    /* instance/duration/max_events 这三个字段才是 ext_adv_start 里要的：没有 period。 */
    esp_ble_gap_ext_adv_t adv = {
        .instance = g_adv_handle,
        .duration = 0,      // 0 = 不限时（直到 stop）
        .max_events = 0,    // 0 = 不限制事件次数
    };

    esp_err_t err = esp_ble_gap_ext_adv_start(1, &adv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_ext_adv_start failed: %s", esp_err_to_name(err));
    }
}

static void stop_ext_adv(void)
{
    /* 注意：这里要传“可取地址的变量/数组”，不能对宏常量取地址 */
    uint8_t inst[1] = { g_adv_handle };
    esp_err_t err = esp_ble_gap_ext_adv_stop(1, inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_ext_adv_stop failed: %s", esp_err_to_name(err));
    }
}

static void update_timer_cb(void *arg)
{
    (void)arg;

    /* 简单做法：停->改数据->启，确保观察上更接近你抓到的“Enable/Remove/Set/Enable”节奏 */
    if (g_adv_data_busy) {
        ESP_LOGW(TAG, "Skip update: adv data config in progress");
        return;
    }

    if (g_total_payloads == 0) {
        ESP_LOGE(TAG, "No valid payloads, stop timer");
        if (g_timer) {
            esp_timer_stop(g_timer);
        }
        return;
    }

    stop_ext_adv();

    g_state = (g_state + 1) % g_total_payloads;
    if (set_adv_data_for_state(g_state) != ESP_OK) {
        ESP_LOGE(TAG, "Update state %d failed", g_state);
        return;
    }
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
        ESP_LOGI(TAG, "SET_PARAMS_COMPLETE: status=%d", param->ext_adv_set_params.status);
        if (param->ext_adv_set_params.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "SET_PARAMS failed, stop timer");
            if (g_timer) {
                esp_timer_stop(g_timer);
            }
            break;
        }
        /* 设置随机地址（对应你抓包的 LE Set Advertising Set Random Address） */
        esp_ble_gap_ext_adv_set_rand_addr(g_adv_handle, g_rand_addr);
        break;

    case ESP_GAP_BLE_EXT_ADV_SET_RAND_ADDR_COMPLETE_EVT:
        ESP_LOGI(TAG, "SET_RAND_ADDR_COMPLETE: status=%d", param->ext_adv_set_rand_addr.status);
        if (param->ext_adv_set_rand_addr.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "SET_RAND_ADDR failed, stop timer");
            if (g_timer) {
                esp_timer_stop(g_timer);
            }
            break;
        }
        /* 初次下发 state0 的 adv data + 空 scan rsp */
        set_adv_data_for_state(g_state);
        break;

    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
        /* 你这里不要用 ext_scan_rsp_data_set；在 5.5.2 下建议用 ext_adv_data_set 取 status（你的编译器也这么提示） */
        ESP_LOGI(TAG, "EXT_ADV_DATA_SET_COMPLETE: status=%d", param->ext_adv_data_set.status);
        if (param->ext_adv_data_set.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Adv data config failed, stop timer");
            g_adv_data_busy = false;
            if (g_timer) {
                esp_timer_stop(g_timer);
            }
            break;
        }
        g_adv_data_busy = false;
        /* 第一次配置完数据后启动广播 */
        start_ext_adv();
        break;

    case ESP_GAP_BLE_EXT_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        /* 某些 IDF 版本复用 ext_adv_data_set 结构体来返回 status */
        ESP_LOGI(TAG, "EXT_SCAN_RSP_DATA_SET_COMPLETE: status=%d", param->ext_adv_data_set.status);
        if (param->ext_adv_data_set.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Scan rsp config failed, stop timer");
            if (g_timer) {
                esp_timer_stop(g_timer);
            }
        }
        break;

    case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "EXT_ADV_START_COMPLETE: status=%d", param->ext_adv_start.status);
        break;

    case ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "EXT_ADV_STOP_COMPLETE: status=%d", param->ext_adv_stop.status);
        break;

    default:
        break;
    }
}

void app_main(void)
{
    g_total_payloads = count_valid_payloads();
    if (g_total_payloads == 0) {
        ESP_LOGE(TAG, "No payloads defined, abort");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));

    /* 设置扩展广播参数（会触发 SET_PARAMS_COMPLETE 事件） */
    ESP_ERROR_CHECK(esp_ble_gap_ext_adv_set_params(g_adv_handle, &g_ext_adv_params));

    /* 定时切换 3 种状态（你可以改成按键/串口命令触发） */
    const esp_timer_create_args_t targs = {
        .callback = &update_timer_cb,
        .name = "adv_update",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &g_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_timer, 800 * 1000)); // 每 800ms 切一次
}
