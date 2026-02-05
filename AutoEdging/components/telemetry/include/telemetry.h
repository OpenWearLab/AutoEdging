#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t ts_ms;
    float pressure_kpa;
    float temp_c;
} telemetry_point_t;

typedef struct {
    telemetry_point_t *buf;
    size_t capacity;
    size_t head;
    size_t count;
    SemaphoreHandle_t lock;
} telemetry_t;

void telemetry_init(telemetry_t *t, telemetry_point_t *buffer, size_t capacity);

void telemetry_push(telemetry_t *t, const telemetry_point_t *p);

bool telemetry_get_latest(telemetry_t *t, telemetry_point_t *out);

size_t telemetry_copy_recent(telemetry_t *t,
                             telemetry_point_t *out,
                             size_t max_points,
                             int64_t since_ms);

#ifdef __cplusplus
}
#endif
