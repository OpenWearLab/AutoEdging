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
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xd1, 0xb2, 0x0a,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xd0, 0x3b, 0x1b,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xd3, 0xa0, 0x29,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xd2, 0x29, 0x38,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xdd, 0xde, 0xc0,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xdc, 0x57, 0xd1,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL( // Stop vibrate
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD5, 0x96, 0x4C,
        0x03, 0x03, 0x8F, 0xAE)
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
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xa1, 0x35, 0x79,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xa0, 0xbc, 0x68,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xa3, 0x27, 0x5a,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xa2, 0xae, 0x4b,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xad, 0x59, 0xb3,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL(
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xac, 0xd0, 0xa2,
        0x03, 0x03, 0x8F, 0xAE),
    ADV_PL( // Stop swing
        0x02, 0x01, 0x01,
        0x0E, 0xFF, 0xFF, 0x00,
        0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xA5, 0x11, 0x3F,
        0x03, 0x03, 0x8F, 0xAE)
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
static bool g_ble_ready = false;
static bool g_timer_started = false;

/* 对外暴露的手动切换接口（group: 0=vibrate, 1=swing；slot: 0-9） */
esp_err_t ext_adv_switch_state(int group, int slot);
esp_err_t ext_adv_init(void);
esp_err_t ext_adv_start_auto(uint64_t period_us);
esp_err_t ext_adv_stop_auto(void);

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

static int flatten_index(int group, int slot)
{
    if (group < 0 || group >= (int)(sizeof(g_groups) / sizeof(g_groups[0]))) {
        return -1;
    }
    if (slot < 0 || slot >= (int)g_groups[group].count) {
        return -1;
    }
    if (g_groups[group].list[slot].data == NULL || g_groups[group].list[slot].len == 0) {
        return -1;
    }

    int idx = 0;
    for (int g = 0; g < (int)(sizeof(g_groups) / sizeof(g_groups[0])); ++g) {
        for (int i = 0; i < (int)g_groups[g].count; ++i) {
            const adv_payload_t *p = &g_groups[g].list[i];
            if (p->data == NULL || p->len == 0) {
                continue;
            }
            if (g == group && i == slot) {
                return idx;
            }
            ++idx;
        }
    }
    return -1;
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

/* 手动切换到指定组/槽的 payload，并停止自动轮询定时器 */
esp_err_t ext_adv_switch_state(int group, int slot)
{
    int idx = flatten_index(group, slot);
    if (idx < 0) {
        ESP_LOGE(TAG, "Invalid group=%d slot=%d", group, slot);
        return ESP_ERR_INVALID_ARG;
    }
    if (g_adv_data_busy) {
        ESP_LOGW(TAG, "Busy, cannot switch now");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_timer) {
        esp_timer_stop(g_timer);
    }

    stop_ext_adv();
    g_state = idx;
    return set_adv_data_for_state(g_state);
}

/* 初始化 BLE 控制器/协议栈、注册 GAP 回调、设置参数并预加载首帧 */
esp_err_t ext_adv_init(void)
{
    if (g_ble_ready) {
        return ESP_OK;
    }

    g_total_payloads = count_valid_payloads();
    if (g_total_payloads == 0) {
        ESP_LOGE(TAG, "No payloads defined, abort");
        return ESP_ERR_INVALID_STATE;
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

    g_ble_ready = true;
    return ESP_OK;
}

/* 启动自动轮播定时器（微秒），已在运行则先停止再启动 */
esp_err_t ext_adv_start_auto(uint64_t period_us)
{
    if (!g_ble_ready) {
        ESP_LOGE(TAG, "Call ext_adv_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_timer && g_timer_started) {
        esp_timer_stop(g_timer);
        g_timer_started = false;
    }

    if (g_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = &update_timer_cb,
            .name = "adv_update",
        };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &g_timer));
    }

    ESP_ERROR_CHECK(esp_timer_start_periodic(g_timer, period_us));
    g_timer_started = true;
    return ESP_OK;
}

esp_err_t ext_adv_stop_auto(void)
{
    if (g_timer && g_timer_started) {
        esp_timer_stop(g_timer);
        g_timer_started = false;
    }
    return ESP_OK;
}

#ifndef EXT_ADV_DEMO_APP_MAIN
#define EXT_ADV_DEMO_APP_MAIN 1
#endif

#if EXT_ADV_DEMO_APP_MAIN
/* Demo 入口，方便本例独立跑通；集成到系统时可以关闭宏或直接移除 */
void app_main(void)
{
    ESP_ERROR_CHECK(ext_adv_init());
    ESP_ERROR_CHECK(ext_adv_start_auto(1000 * 1000)); // 每 1000ms 切一次

}
#endif
