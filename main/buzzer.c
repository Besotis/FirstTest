#include "buzzer.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BUZZER_GPIO             15

#define BUZZER_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER       LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_1
#define BUZZER_DUTY_RES         LEDC_TIMER_10_BIT
#define BUZZER_DUTY_MAX         ((1U << 10) - 1U)

#define BUZZER_QUEUE_LEN        8
#define BUZZER_TASK_STACK       3072
#define BUZZER_TASK_PRIO        4

static const char *TAG = "BUZZER";

typedef enum {
    BUZZER_CMD_CLICK = 0,
    BUZZER_CMD_STARTUP,
} buzzer_cmd_t;

static QueueHandle_t buzzer_queue = NULL;
static bool buzzer_ready = false;

/* Default boot volume. SquareLine slider overrides this after UI init. */
static uint8_t volume_percent = 55;

static uint32_t volume_to_duty(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    /*
     * Passive piezo is strongest near 50% square-wave duty.
     * 100% user volume maps to 50% hardware PWM duty.
     */
    return ((BUZZER_DUTY_MAX / 2U) * percent) / 100U;
}

static void buzzer_off(void)
{
    if (!buzzer_ready) {
        return;
    }

    ESP_ERROR_CHECK(
        ledc_set_duty(
            BUZZER_LEDC_MODE,
            BUZZER_LEDC_CHANNEL,
            0
        )
    );

    ESP_ERROR_CHECK(
        ledc_update_duty(
            BUZZER_LEDC_MODE,
            BUZZER_LEDC_CHANNEL
        )
    );
}

static void buzzer_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!buzzer_ready || frequency_hz == 0 || duration_ms == 0) {
        return;
    }

    if (volume_percent == 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }

    ESP_ERROR_CHECK(
        ledc_set_freq(
            BUZZER_LEDC_MODE,
            BUZZER_LEDC_TIMER,
            frequency_hz
        )
    );

    ESP_ERROR_CHECK(
        ledc_set_duty(
            BUZZER_LEDC_MODE,
            BUZZER_LEDC_CHANNEL,
            volume_to_duty(volume_percent)
        )
    );

    ESP_ERROR_CHECK(
        ledc_update_duty(
            BUZZER_LEDC_MODE,
            BUZZER_LEDC_CHANNEL
        )
    );

    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    buzzer_off();
}

static void play_click(void)
{
    /* Short UI tick */
    buzzer_tone(1450, 25);
}

static void play_startup(void)
{
    /* "cip cip" */
    buzzer_tone(1050, 55);
    vTaskDelay(pdMS_TO_TICKS(45));
    buzzer_tone(1650, 70);
}

static void buzzer_task(void *arg)
{
    (void)arg;

    buzzer_cmd_t cmd;

    while (1) {
        if (xQueueReceive(
                buzzer_queue,
                &cmd,
                portMAX_DELAY
            ) == pdTRUE) {

            switch (cmd) {
                case BUZZER_CMD_CLICK:
                    play_click();
                    break;

                case BUZZER_CMD_STARTUP:
                    play_startup();
                    break;

                default:
                    break;
            }
        }
    }
}

static void queue_sound(buzzer_cmd_t cmd)
{
    if (!buzzer_queue) {
        return;
    }

    /* Never block LVGL/navigation. */
    (void)xQueueSend(buzzer_queue, &cmd, 0);
}

void buzzer_set_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    volume_percent = percent;

    /* 0% = immediate mute */
    if (volume_percent == 0) {
        buzzer_off();
    }
}

uint8_t buzzer_get_volume(void)
{
    return volume_percent;
}

void buzzer_click(void)
{
    queue_sound(BUZZER_CMD_CLICK);
}

void buzzer_startup(void)
{
    queue_sound(BUZZER_CMD_STARTUP);
}

/* ---------------- Volume slider integration ---------------- */

static uint8_t slider_percent(lv_obj_t *slider)
{
    int32_t value = lv_slider_get_value(slider);

    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }

    return (uint8_t)value;
}

static void volume_changed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    buzzer_set_volume(
        slider_percent(lv_event_get_target(e))
    );
}

static void volume_released_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }

    uint8_t percent =
        slider_percent(lv_event_get_target(e));

    buzzer_set_volume(percent);

    ESP_LOGI(
        TAG,
        "Volume=%u%%",
        (unsigned)percent
    );

    /*
     * Preview selected level using the startup "cip cip".
     * At 0% buzzer_startup() stays silent.
     */
    buzzer_startup();
}

void buzzer_volume_slider_init(lv_obj_t *slider)
{
    if (!slider) {
        ESP_LOGE(TAG, "Volume slider is NULL");
        return;
    }

    lv_slider_set_range(slider, 0, 100);

    uint8_t initial = slider_percent(slider);
    buzzer_set_volume(initial);

    /*
     * Keep slider interaction stable if the finger drifts slightly outside
     * the knob while dragging.
     */
    lv_obj_add_flag(slider, LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_add_event_cb(
        slider,
        volume_changed_cb,
        LV_EVENT_VALUE_CHANGED,
        NULL
    );

    lv_obj_add_event_cb(
        slider,
        volume_released_cb,
        LV_EVENT_RELEASED,
        NULL
    );

    ESP_LOGI(
        TAG,
        "Volume slider ready: %u%%",
        (unsigned)initial
    );
}

void buzzer_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    buzzer_ready = true;
    buzzer_off();

    buzzer_queue =
        xQueueCreate(
            BUZZER_QUEUE_LEN,
            sizeof(buzzer_cmd_t)
        );

    if (!buzzer_queue) {
        ESP_LOGE(TAG, "Failed to create buzzer queue");
        return;
    }

    BaseType_t task_ok =
        xTaskCreate(
            buzzer_task,
            "buzzer",
            BUZZER_TASK_STACK,
            NULL,
            BUZZER_TASK_PRIO,
            NULL
        );

    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create buzzer task");
        return;
    }

    ESP_LOGI(
        TAG,
        "Passive buzzer ready: GPIO=%d volume=%u%%",
        BUZZER_GPIO,
        (unsigned)volume_percent
    );
}
