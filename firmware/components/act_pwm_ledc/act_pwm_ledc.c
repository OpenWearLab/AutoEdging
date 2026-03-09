#include "act_pwm_ledc.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pwm_ledc";

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static esp_err_t apply_channel_duty(const pwm_ledc_t *pwm, const pwm_ledc_channel_config_t *ch, uint32_t duty_raw)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(pwm->cfg.timer_cfg.speed_mode, ch->channel, duty_raw), TAG, "set duty");
    // set_duty 后必须 update_duty 才生效
    ESP_RETURN_ON_ERROR(ledc_update_duty(pwm->cfg.timer_cfg.speed_mode, ch->channel), TAG, "update duty");
    return ESP_OK;
}

esp_err_t pwm_ledc_init(pwm_ledc_t *out, const pwm_ledc_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(out && cfg, ESP_ERR_INVALID_ARG, TAG, "null arg");

    *out = (pwm_ledc_t){0};
    out->cfg = *cfg;

    // 1) choose duty resolution
    ledc_timer_bit_t res = cfg->timer_cfg.duty_resolution;
    if (res == 0) {
        // 用 helper 自动选“尽量高”的 duty 分辨率（受频率/时钟限制）
        uint32_t src_clk = cfg->timer_cfg.src_clk_hz;
        if (src_clk == 0) {
            // 不强行指定 src_clk_hz 时，仍可给个常见基准；实际硬件由 LEDC 选择时钟
            // 这里用 80MHz 作为常见 APB 频率的近似值（若你想更严谨，可改为从 TRM/SoC 获取）
            src_clk = 80000000;
        }
        uint32_t res_bits = ledc_find_suitable_duty_resolution(src_clk, cfg->timer_cfg.pwm_freq_hz);
        res = (ledc_timer_bit_t)res_bits;
    }
    out->resolution = res;
    out->duty_max = (res >= 1 && res <= LEDC_TIMER_14_BIT) ? ((1u << res) - 1u) : 0;

    ESP_RETURN_ON_FALSE(out->duty_max > 0, ESP_ERR_INVALID_ARG, TAG, "bad duty resolution");

    // 2) timer config (先 timer 再 channel)
    ledc_timer_config_t tcfg = {
        .speed_mode = cfg->timer_cfg.speed_mode,
        .duty_resolution = res,
        .timer_num = cfg->timer_cfg.timer,
        .freq_hz = cfg->timer_cfg.pwm_freq_hz,
        .clk_cfg = cfg->timer_cfg.clk_cfg,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "timer config");

    // 3) channel config
    for (int i = 0; i < 4; i++) {
        const pwm_ledc_channel_config_t *ch = &cfg->ch_cfg[i];

        ESP_RETURN_ON_FALSE(ch->motor_id == i, ESP_ERR_INVALID_ARG, TAG, "motor_id must be 0..3 in order");
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(ch->gpio), ESP_ERR_INVALID_ARG, TAG, "invalid gpio");

        uint32_t permille = clamp_u32(ch->init_duty_permille, 0, 1000);
        uint32_t duty_raw = (permille * out->duty_max) / 1000u;

        ledc_channel_config_t ccfg = {
            .gpio_num = ch->gpio,
            .speed_mode = cfg->timer_cfg.speed_mode,
            .channel = ch->channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = cfg->timer_cfg.timer,
            .duty = duty_raw,
            .hpoint = 0,
            .flags.output_invert = ch->output_invert ? 1 : 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "channel config");
    }

    out->inited = true;
    ESP_LOGI(TAG, "init ok: freq=%" PRIu32 "Hz res=%d duty_max=%" PRIu32,
             cfg->timer_cfg.pwm_freq_hz, (int)res, out->duty_max);
    return ESP_OK;
}

esp_err_t pwm_ledc_set_permille(pwm_ledc_t *pwm, int motor_id, uint32_t permille)
{
    ESP_RETURN_ON_FALSE(pwm && pwm->inited, ESP_ERR_INVALID_STATE, TAG, "not inited");
    ESP_RETURN_ON_FALSE(motor_id >= 0 && motor_id < 4, ESP_ERR_INVALID_ARG, TAG, "bad motor id");

    permille = clamp_u32(permille, 0, 1000);
    uint32_t duty_raw = (permille * pwm->duty_max) / 1000u;

    const pwm_ledc_channel_config_t *ch = &pwm->cfg.ch_cfg[motor_id];
    return apply_channel_duty(pwm, ch, duty_raw);
}

esp_err_t pwm_ledc_stop(pwm_ledc_t *pwm, int motor_id, uint32_t idle_level)
{
    ESP_RETURN_ON_FALSE(pwm && pwm->inited, ESP_ERR_INVALID_STATE, TAG, "not inited");
    ESP_RETURN_ON_FALSE(motor_id >= 0 && motor_id < 4, ESP_ERR_INVALID_ARG, TAG, "bad motor id");
    idle_level = idle_level ? 1 : 0;

    const pwm_ledc_channel_config_t *ch = &pwm->cfg.ch_cfg[motor_id];
    return ledc_stop(pwm->cfg.timer_cfg.speed_mode, ch->channel, idle_level);
}
