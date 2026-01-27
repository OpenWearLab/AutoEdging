#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

static const char *TAG = "EXT_ADV_REPLAY";

/* 你抓包里怀疑的 Advertising Handle */
static uint8_t g_adv_handle = 0x02;

/* 你抓到的 “Set Random Address” 对应的随机地址（示例随便写一个）。
 * 如果你想完全复刻手机行为，也可以把抓到的随机地址填进来。 */
static uint8_t g_rand_addr[6] = {0xC2, 0x11, 0x22, 0x33, 0x44, 0x55};

/* 你抓到的 22 bytes 广播数据（Frame 473/493 里的 Manufacturer Specific + UUID 列表），
 * 这里用占位。你把 data: 6db643ce97fe427cd7846f... + 后面的 ae8f 之类按你想要的方式组织成 raw AD 即可。
 *
 * 注意：raw AD 是“AD structure”序列： [len][type][...payload...] [len][type][...]
 * 你抓到的 Wireshark 解析已经是 AD 结构了：Flags + Manufacturer Specific + 16-bit UUIDs。
 * 所以你应当把这三段按 raw 形式拼起来，而不是只填 manufacturer data。
 */
static const uint8_t adv_payload_state0[] = {
    /* Flags: len=2, type=0x01, data=0x01(LE Limited Discoverable) -> 0x01 */
    0x02, 0x01, 0x01,

    /* Manufacturer Specific: len=14, type=0xFF, company_id=0x00FF */
    0x0E, 0xFF, 0xFF, 0x00,
    
    /* data=11 bytes(占位) */
    0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD4, 0x1F, 0x5D,

    /* 16-bit Service Class UUIDs: len=3, type=0x03, UUID=0xAE8F */
    0x03, 0x03, 0x8F, 0xAE,
};

static const uint8_t adv_payload_state1[] = {
    0x02, 0x01, 0x01,
    0x0E, 0xFF, 0xFF, 0x00,
    0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD7, 0x84, 0x6F,
    0x03, 0x03, 0x8F, 0xAE,
};

static const uint8_t adv_payload_state2[] = {
    0x02, 0x01, 0x01,
    0x0E, 0xFF, 0xFF, 0x00,
    0x6D, 0xB6, 0x43, 0xCE, 0x97, 0xFE, 0x42, 0x7C, 0xD6, 0x0D, 0x7E,
    0x03, 0x03, 0x8F, 0xAE,
};

static int g_state = 0;

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

static void set_adv_data_for_state(int state)
{
    const uint8_t *p = NULL;
    uint16_t len = 0;

    if (state == 0) { p = adv_payload_state0; len = sizeof(adv_payload_state0); }
    else if (state == 1) { p = adv_payload_state1; len = sizeof(adv_payload_state1); }
    else { p = adv_payload_state2; len = sizeof(adv_payload_state2); }

    ESP_LOGI(TAG, "Config ext adv data: handle=0x%02x, state=%d, len=%u", g_adv_handle, state, (unsigned)len);

    /* raw 扩展广播数据 */
    /* API signature: (instance, length, data) */
    esp_err_t err = esp_ble_gap_config_ext_adv_data_raw(g_adv_handle, len, p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_ext_adv_data_raw failed: %s", esp_err_to_name(err));
    }

    /* raw 扫描响应数据（你抓包是 length=0） */
    /* length = 0, data = NULL to express "no scan response" */
    err = esp_ble_gap_config_ext_scan_rsp_data_raw(g_adv_handle, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_config_ext_scan_rsp_data_raw failed: %s", esp_err_to_name(err));
    }
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
    stop_ext_adv();

    g_state = (g_state + 1) % 3;
    set_adv_data_for_state(g_state);

    start_ext_adv();
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
        ESP_LOGI(TAG, "SET_PARAMS_COMPLETE: status=%d", param->ext_adv_set_params.status);
        /* 设置随机地址（对应你抓包的 LE Set Advertising Set Random Address） */
        esp_ble_gap_ext_adv_set_rand_addr(g_adv_handle, g_rand_addr);
        break;

    case ESP_GAP_BLE_EXT_ADV_SET_RAND_ADDR_COMPLETE_EVT:
        ESP_LOGI(TAG, "SET_RAND_ADDR_COMPLETE: status=%d", param->ext_adv_set_rand_addr.status);
        /* 初次下发 state0 的 adv data + 空 scan rsp */
        set_adv_data_for_state(g_state);
        break;

    case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_EXT_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        /* 你这里不要用 ext_scan_rsp_data_set；在 5.5.2 下建议用 ext_adv_data_set 取 status（你的编译器也这么提示） */
        ESP_LOGI(TAG, "EXT_ADV/SCAN_RSP_DATA_SET_COMPLETE: status=%d", param->ext_adv_data_set.status);
        /* 第一次配置完数据后启动广播 */
        start_ext_adv();
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
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 800 * 1000)); // 每 800ms 切一次
}
