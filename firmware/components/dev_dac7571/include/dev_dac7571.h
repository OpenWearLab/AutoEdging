#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef enum {
    DAC7571_PD_NORMAL = 0,   // 00
    DAC7571_PD_1K     = 1,   // 01
    DAC7571_PD_100K   = 2,   // 10
    DAC7571_PD_HIZ    = 3,   // 11
} dac7571_pd_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    uint16_t last_code;
    dac7571_pd_t last_pd;
} dac7571_t;

typedef struct {
    uint8_t i2c_addr_7bit;   // 0x4C or 0x4D depending on A0
    uint32_t scl_speed_hz;   // e.g. 400000
} dac7571_config_t;

esp_err_t dac7571_init(dac7571_t *out, i2c_master_bus_handle_t bus, const dac7571_config_t *cfg);
esp_err_t dac7571_write(dac7571_t *h, uint16_t code_12bit, dac7571_pd_t pd_mode);
