#include "bus_i2c.h"
#include "esp_check.h"

esp_err_t bus_i2c_init(bus_i2c_t *out, const bus_i2c_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(out && cfg, ESP_ERR_INVALID_ARG, "bus_i2c", "null arg");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->port,
        .sda_io_num = cfg->sda_io,
        .scl_io_num = cfg->scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = cfg->glitch_ignore_cnt,
        .intr_priority = 0,
        .flags.enable_internal_pullup = cfg->enable_internal_pullups,
    };

    return i2c_new_master_bus(&bus_cfg, &out->bus);
}

void bus_i2c_deinit(bus_i2c_t *bus)
{
    if (bus && bus->bus) {
        i2c_del_master_bus(bus->bus);
        bus->bus = NULL;
    }
}
