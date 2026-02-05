#include "dev_mcp_h11.h"
#include "esp_check.h"

#define MCP_H11_REG_STATUS    0xD0
#define MCP_H11_REG_P_MSB     0xD1
#define MCP_H11_REG_T_MSB     0xD4
#define MCP_H11_REG_MEAS_CFG  0xDE

static int32_t sign_extend_24(uint32_t v)
{
    v &= 0x00FFFFFFu;
    if (v & 0x00800000u) {
        v |= 0xFF000000u;
    }
    return (int32_t)v;
}

esp_err_t mcp_h11_init(mcp_h11_t *out, i2c_master_bus_handle_t bus, const mcp_h11_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(out && bus && cfg, ESP_ERR_INVALID_ARG, "mcp_h11", "null arg");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c_addr_7bit,
        .scl_speed_hz = cfg->scl_speed_hz,
        .scl_wait_us = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &out->dev), "mcp_h11", "add device failed");
    out->a = cfg->a;
    out->b = cfg->b;
    return ESP_OK;
}

esp_err_t mcp_h11_read_regs(const mcp_h11_t *h, uint8_t reg, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(h && h->dev && buf && len > 0, ESP_ERR_INVALID_ARG, "mcp_h11", "bad arg");
    return i2c_master_transmit_receive(h->dev, &reg, 1, buf, len, 100 /*ms*/);
}

esp_err_t mcp_h11_write_reg(const mcp_h11_t *h, uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_FALSE(h && h->dev, ESP_ERR_INVALID_ARG, "mcp_h11", "bad arg");
    uint8_t w[2] = { reg, val };
    return i2c_master_transmit(h->dev, w, sizeof(w), 100 /*ms*/);
}

esp_err_t mcp_h11_write_meas_cfg(const mcp_h11_t *h, uint8_t meas_cfg)
{
    // datasheet sample writes 0xF8 to 0xDE then delays; keep it configurable
    return mcp_h11_write_reg(h, MCP_H11_REG_MEAS_CFG, meas_cfg);
}

esp_err_t mcp_h11_read_sample(const mcp_h11_t *h, mcp_h11_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, "mcp_h11", "bad arg");

    // Read D0..D5 in one shot: status + P(3) + T(2)
    uint8_t buf[6] = {0};
    ESP_RETURN_ON_ERROR(mcp_h11_read_regs(h, MCP_H11_REG_STATUS, buf, sizeof(buf)), "mcp_h11", "read failed");

    out->status = buf[0];

    uint32_t p_u24 = ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
    out->p_code = sign_extend_24(p_u24);

    uint16_t t_u16 = ((uint16_t)buf[4] << 8) | (uint16_t)buf[5];
    out->temp_c = (int16_t)t_u16;

    // Pressure transfer function: P(kPa) = A * PCode / 8388607 + B
    float p = (h->a * ((float)out->p_code) / 8388607.0f) + h->b;
    if (p < 0.0f) p = 0.0f;
    out->pressure_kpa = p;

    return ESP_OK;
}
