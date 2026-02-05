#pragma once

#include "esp_err.h"

#include "control_api.h"
#include "telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    control_service_t *control;
    telemetry_t *telemetry;
} web_server_ctx_t;

esp_err_t web_server_start(const web_server_ctx_t *ctx);
void web_server_stop(void);

#ifdef __cplusplus
}
#endif
