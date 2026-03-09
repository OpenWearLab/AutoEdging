#include "dev_dac7571.h"
#include "esp_check.h"

esp_err_t dac7571_init(dac7571_t *out, i2c_master_bus_handle_t bus, const dac7571_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(out && bus && cfg, ESP_ERR_INVALID_ARG, "dac7571", "null arg");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c_addr_7bit,
        .scl_speed_hz = cfg->scl_speed_hz,
        .scl_wait_us = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &out->dev), "dac7571", "add device failed");
    out->last_code = 0;
    out->last_pd = DAC7571_PD_NORMAL;
    return ESP_OK;
}

esp_err_t dac7571_write(dac7571_t *h, uint16_t code_12bit, dac7571_pd_t pd_mode)
{
    ESP_RETURN_ON_FALSE(h && h->dev, ESP_ERR_INVALID_ARG, "dac7571", "bad arg");
    code_12bit &= 0x0FFFu;

    // Ctrl/MS-Byte: 0 0 PD1 PD0 D11 D10 D9 D8
    uint8_t msb = ((uint8_t)(pd_mode & 0x03u) << 4) | (uint8_t)((code_12bit >> 8) & 0x0Fu);
    uint8_t lsb = (uint8_t)(code_12bit & 0xFFu);

    uint8_t w[2] = { msb, lsb };
    esp_err_t err = i2c_master_transmit(h->dev, w, sizeof(w), 100 /*ms*/);
    if (err == ESP_OK) {
        h->last_code = code_12bit;
        h->last_pd = pd_mode;
    }
    return err;
}
