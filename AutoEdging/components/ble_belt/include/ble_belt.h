#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 BLE 与扩展广播；内部会启动一次默认广播（payload 0）
esp_err_t ble_belt_init(void);

// 发送写死的广播包（0-based：和你示例 g_vibrate_payloads[1] 一致）
esp_err_t ble_belt_send_vibrate(int idx); // idx: 0..9
esp_err_t ble_belt_send_swing(int idx);   // idx: 0..9

#ifdef __cplusplus
}
#endif
