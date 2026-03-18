#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 BLE 与 legacy 广播能力；首次发送命令时才会启动广播
esp_err_t ble_belt_init(void);

// 发送写死的广播包（0-based：和你示例 g_vibrate_payloads[1] 一致）
esp_err_t ble_belt_send_vibrate(int idx); // idx: 0..9
esp_err_t ble_belt_send_swing(int idx);   // idx: 0..9

#ifdef __cplusplus
}
#endif
