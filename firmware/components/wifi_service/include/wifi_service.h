#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool provisioned;
    bool connected;
    bool provisioning;
    bool connect_failed;
    bool reboot_pending;
    char service_name[32];
    char ssid[33];
    char ip_addr[16];
} wifi_service_status_t;

esp_err_t wifi_service_start(void);
void wifi_service_get_status(wifi_service_status_t *out);
bool wifi_service_is_connected(void);
bool wifi_service_has_failed(void);
esp_err_t wifi_service_request_reprovision_reboot(void);

#ifdef __cplusplus
}
#endif
