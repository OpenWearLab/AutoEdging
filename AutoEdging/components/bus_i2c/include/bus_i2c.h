#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_bus_handle_t bus;
} bus_i2c_t;

typedef struct {
    i2c_port_num_t port;              // I2C_NUM_0 / I2C_NUM_1, or -1 for auto select
    gpio_num_t sda_io;
    gpio_num_t scl_io;
    uint32_t clk_speed_hz;            // e.g. 400000
    bool enable_internal_pullups;     // still recommend external pullups
    uint8_t glitch_ignore_cnt;        // typical 7
} bus_i2c_config_t;

esp_err_t bus_i2c_init(bus_i2c_t *out, const bus_i2c_config_t *cfg);
void bus_i2c_deinit(bus_i2c_t *bus);
