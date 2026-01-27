#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "driver/rmt_tx.h"
#include "led_strip.h"

#include "bus_i2c.h"
#include "ble_belt.h"
#include "dev_mcp_h11.h"
#include "dev_dac7571.h"
#include "act_pwm_ledc.h"


static const char *TAG = "app_main";

#define APP_I2C_PORT        I2C_NUM_0
#define APP_I2C_SDA_GPIO    GPIO_NUM_5
#define APP_I2C_SCL_GPIO    GPIO_NUM_4
#define APP_I2C_FREQ_HZ     400000

#define BLINK_GPIO          GPIO_NUM_38

#define M0_GPIO             GPIO_NUM_10
#define M1_GPIO             GPIO_NUM_11
#define M2_GPIO             GPIO_NUM_12
#define M3_GPIO             GPIO_NUM_13

static void app_init_devices(led_strip_handle_t *led_strip, mcp_h11_t *mcp, dac7571_t *dac, pwm_ledc_t *motors)
{
    // 0) init LED strip
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, led_strip));
    ESP_ERROR_CHECK(led_strip_clear(*led_strip));

    // 1) init I2C bus
    bus_i2c_t i2c = {0};
    bus_i2c_config_t bus_cfg = {
        .port = APP_I2C_PORT,
        .sda_io = APP_I2C_SDA_GPIO,
        .scl_io = APP_I2C_SCL_GPIO,
        .clk_speed_hz = APP_I2C_FREQ_HZ,
        .enable_internal_pullups = true,
        .glitch_ignore_cnt = 7,
    };
    ESP_ERROR_CHECK(bus_i2c_init(&i2c, &bus_cfg));

    // 2) init MCP-H11
    mcp_h11_config_t mcp_cfg = {
        .i2c_addr_7bit = 0x36,          // MCP-H11 datasheet: 7-bit 0x36
        .scl_speed_hz = APP_I2C_FREQ_HZ,
        .a = 50.0f,
        .b = -5.0f,
    };
    ESP_ERROR_CHECK(mcp_h11_init(mcp, i2c.bus, &mcp_cfg));

    // 3) init DAC7571
    dac7571_config_t dac_cfg = {
        .i2c_addr_7bit = 0x4D,          // DAC7571: A0=0 => 0x4C；A0=1 => 0x4D
        .scl_speed_hz = APP_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(dac7571_init(dac, i2c.bus, &dac_cfg));

    // 4) init motor PWM
    pwm_ledc_config_t pwm_cfg = {
        .timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer = LEDC_TIMER_0,
            .pwm_freq_hz = 20000,             // 电机常用 20kHz，按你的驱动/电机调整
            .clk_cfg = LEDC_AUTO_CLK,
            .src_clk_hz = 80000000,           // 用于自动选分辨率的参考
            .duty_resolution = 0,             // 0 => 自动选择
        },
        .ch_cfg = {
            { .motor_id=0, .gpio=M0_GPIO, .channel=LEDC_CHANNEL_0, .init_duty_permille=0, .output_invert=false },
            { .motor_id=1, .gpio=M1_GPIO, .channel=LEDC_CHANNEL_1, .init_duty_permille=0, .output_invert=false },
            { .motor_id=2, .gpio=M2_GPIO, .channel=LEDC_CHANNEL_2, .init_duty_permille=0, .output_invert=false },
            { .motor_id=3, .gpio=M3_GPIO, .channel=LEDC_CHANNEL_3, .init_duty_permille=0, .output_invert=false },
        },
    };
    ESP_ERROR_CHECK(pwm_ledc_init(motors, &pwm_cfg));

    // 5) init BLE belt
    ESP_ERROR_CHECK(ble_belt_init());
}

static esp_err_t nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    ESP_LOGI(TAG, "====== Boot ======");

    ESP_ERROR_CHECK(nvs_init());

    led_strip_handle_t led_strip = NULL;
    mcp_h11_t mcp_h11 = {0};
    dac7571_t dac7571 = {0};
    pwm_ledc_t motors = {0};

    app_init_devices(&led_strip, &mcp_h11, &dac7571, &motors);

    uint16_t dac_code = 0;
    static uint32_t sp = 0;
    uint16_t ble_belt = 0;
    while (1) {
        mcp_h11_sample_t s = {0};
        esp_err_t err = mcp_h11_read_sample(&mcp_h11, &s);

        if (err == ESP_OK) {
            // 先用锯齿波验证 DAC 写入稳定
            dac_code = (dac_code + 0x010) & 0x0FFF;
            ESP_ERROR_CHECK(dac7571_write(&dac7571, dac_code, DAC7571_PD_NORMAL));

            ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, 0, s.pressure_kpa*10, 0, 0));
            ESP_ERROR_CHECK(led_strip_refresh(led_strip));

            ESP_LOGI(TAG,
                     "status=0x%02X p=%.3f kPa t=%.2f C dac=%.3f V",
                     s.status, (double)s.pressure_kpa, (double)s.temp_c, (dac_code/4095.0*3.3));
        } else {
            ESP_LOGW(TAG, "mcp read failed: %s", esp_err_to_name(err));
        }

        sp = (sp + 20) % 500; // 0..499
        ESP_ERROR_CHECK(pwm_ledc_set_permille(&motors, 0, sp));
        ESP_ERROR_CHECK(pwm_ledc_set_permille(&motors, 1, sp));
        ESP_ERROR_CHECK(pwm_ledc_set_permille(&motors, 2, sp));
        ESP_ERROR_CHECK(pwm_ledc_set_permille(&motors, 3, sp));

        ble_belt = (ble_belt + 3) % 10;
        ESP_ERROR_CHECK(ble_belt_send_swing(ble_belt));
        ESP_ERROR_CHECK(ble_belt_send_vibrate(ble_belt));

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
