#include "brightness.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

#define BRIGHTNESS_PWM_FREQ_HZ       5000
#define BRIGHTNESS_DUTY_RES          LEDC_TIMER_10_BIT
#define BRIGHTNESS_DUTY_MAX          ((1U << 10) - 1U)

#define BRIGHTNESS_LEDC_MODE         LEDC_LOW_SPEED_MODE
#define BRIGHTNESS_LEDC_TIMER        LEDC_TIMER_0
#define BRIGHTNESS_LEDC_CHANNEL      LEDC_CHANNEL_0

#define BRIGHTNESS_MIN_PERCENT       10

static const char *TAG = "BRIGHTNESS";

static bool brightness_ready = false;
static uint8_t current_percent = 50;
static lv_obj_t *brightness_slider = NULL;

static uint32_t percent_to_duty(uint8_t percent)
{
    return ((uint32_t)percent * BRIGHTNESS_DUTY_MAX) / 100U;
}

void brightness_set(uint8_t percent)
{
    if (percent < BRIGHTNESS_MIN_PERCENT) {
        percent = BRIGHTNESS_MIN_PERCENT;
    }

    if (percent > 100) {
        percent = 100;
    }

    current_percent = percent;

    if (!brightness_ready) {
        return;
    }

    ESP_ERROR_CHECK(
        ledc_set_duty(BRIGHTNESS_LEDC_MODE,
                      BRIGHTNESS_LEDC_CHANNEL,
                      percent_to_duty(percent))
    );

    ESP_ERROR_CHECK(
        ledc_update_duty(BRIGHTNESS_LEDC_MODE,
                         BRIGHTNESS_LEDC_CHANNEL)
    );
}

uint8_t brightness_get(void)
{
    return current_percent;
}

static void brightness_slider_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    int32_t value = lv_slider_get_value(lv_event_get_target(e));

    if (value < BRIGHTNESS_MIN_PERCENT) {
        value = BRIGHTNESS_MIN_PERCENT;
    } else if (value > 100) {
        value = 100;
    }

    brightness_set((uint8_t)value);
}

void brightness_init(int gpio_num, lv_obj_t *slider)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BRIGHTNESS_LEDC_MODE,
        .duty_resolution = BRIGHTNESS_DUTY_RES,
        .timer_num = BRIGHTNESS_LEDC_TIMER,
        .freq_hz = BRIGHTNESS_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {
        .gpio_num = gpio_num,
        .speed_mode = BRIGHTNESS_LEDC_MODE,
        .channel = BRIGHTNESS_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BRIGHTNESS_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    brightness_ready = true;
    brightness_slider = slider;

    if (brightness_slider) {
        lv_slider_set_range(brightness_slider,
                            BRIGHTNESS_MIN_PERCENT,
                            100);

        int32_t value = lv_slider_get_value(brightness_slider);

        if (value < BRIGHTNESS_MIN_PERCENT) {
            value = BRIGHTNESS_MIN_PERCENT;
            lv_slider_set_value(brightness_slider,
                                value,
                                LV_ANIM_OFF);
        }

        current_percent = (uint8_t)value;

        lv_obj_add_flag(brightness_slider, LV_OBJ_FLAG_PRESS_LOCK);

        lv_obj_add_event_cb(brightness_slider,
                            brightness_slider_cb,
                            LV_EVENT_VALUE_CHANGED,
                            NULL);
    }

    brightness_set(current_percent);

    ESP_LOGI(TAG,
             "Backlight PWM ready: GPIO=%d, brightness=%u%%",
             gpio_num,
             (unsigned)current_percent);
}
