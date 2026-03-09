#include "telemetry.h"

#include <string.h>

void telemetry_init(telemetry_t *t, telemetry_point_t *buffer, size_t capacity)
{
    if (!t || !buffer || capacity == 0) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->buf = buffer;
    t->capacity = capacity;
    t->head = 0;
    t->count = 0;
    t->lock = xSemaphoreCreateMutex();
}

void telemetry_push(telemetry_t *t, const telemetry_point_t *p)
{
    if (!t || !p || !t->buf || t->capacity == 0) {
        return;
    }
    if (!t->lock || xSemaphoreTake(t->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    t->buf[t->head] = *p;
    t->head = (t->head + 1) % t->capacity;
    if (t->count < t->capacity) {
        t->count++;
    }
    xSemaphoreGive(t->lock);
}

bool telemetry_get_latest(telemetry_t *t, telemetry_point_t *out)
{
    if (!t || !out || !t->buf || t->count == 0) {
        return false;
    }
    if (!t->lock || xSemaphoreTake(t->lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    size_t idx = (t->head + t->capacity - 1) % t->capacity;
    *out = t->buf[idx];
    xSemaphoreGive(t->lock);
    return true;
}

size_t telemetry_copy_recent(telemetry_t *t,
                             telemetry_point_t *out,
                             size_t max_points,
                             int64_t since_ms)
{
    if (!t || !out || !t->buf || max_points == 0) {
        return 0;
    }
    if (!t->lock || xSemaphoreTake(t->lock, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t copied = 0;
    size_t start = (t->head + t->capacity - t->count) % t->capacity;
    for (size_t i = 0; i < t->count; i++) {
        size_t idx = (start + i) % t->capacity;
        if (t->buf[idx].ts_ms >= since_ms) {
            if (copied < max_points) {
                out[copied++] = t->buf[idx];
            }
        }
    }

    xSemaphoreGive(t->lock);
    return copied;
}
