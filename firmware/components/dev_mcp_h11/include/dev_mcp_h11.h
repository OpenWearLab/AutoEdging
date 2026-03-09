#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_dev_handle_t dev;
    float a;   // transfer function coefficient A
    float b;   // transfer function coefficient B
} mcp_h11_t;

typedef struct {
    uint8_t i2c_addr_7bit;   // expected 0x36 per datasheet
    uint32_t scl_speed_hz;   // e.g. 400000
    float a;
    float b;
} mcp_h11_config_t;

typedef struct {
    uint8_t status;       // raw status byte (if you read D0)
    int32_t p_code;       // signed 24-bit extended to int32
    int16_t t_code;       // signed 16-bit
    float pressure_kpa;   // computed from A/B
    float temp_c;         // computed as t_code / 256.0
} mcp_h11_sample_t;

esp_err_t mcp_h11_init(mcp_h11_t *out, i2c_master_bus_handle_t bus, const mcp_h11_config_t *cfg);

// low-level register access
esp_err_t mcp_h11_read_regs(const mcp_h11_t *h, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t mcp_h11_write_reg(const mcp_h11_t *h, uint8_t reg, uint8_t val);

// high-level
esp_err_t mcp_h11_read_sample(const mcp_h11_t *h, mcp_h11_sample_t *out);

// optional: trigger single measurement if your mode requires it (value is chip-specific)
esp_err_t mcp_h11_write_meas_cfg(const mcp_h11_t *h, uint8_t meas_cfg);
