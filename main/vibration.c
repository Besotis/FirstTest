#include "vibration.h"

#include <stdbool.h>
#include <stdint.h>

#include "buzzer.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define VIBRATION_GPIO         16

#define VIBRATION_QUEUE_LEN    8
#define VIBRATION_TASK_STACK   3072
#define VIBRATION_TASK_PRIO    4

static const char *TAG = "VIBRATION";

typedef enum {
    VIBRATION_CMD_CLICK = 0,
    VIBRATION_CMD_STARTUP,
} vibration_cmd_t;

static QueueHandle_t vibration_queue = NULL;
static bool vibration_ready = false;

/* Boot state: ON */
static bool vibration_enabled = true;

static void motor_off(void)
{
    if (!vibration_ready) {
        return;
    }

    gpio_set_level(VIBRATION_GPIO, 0);
}

static void motor_on(void)
{
    if (!vibration_ready || !vibration_enabled) {
        return;
    }

    gpio_set_level(VIBRATION_GPIO, 1);
}

static void vibration_pulse(uint32_t duration_ms)
{
    if (!vibration_ready || !vibration_enabled || duration_ms == 0) {
        return;
    }

    motor_on();
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    motor_off();
}

static void play_click(void)
{
    /*
     * 40 ms was too short to feel reliably with this module.
     * 110 ms gives a much clearer haptic button press.
     */
    vibration_pulse(200);
}

static void play_startup(void)
{
    /* Two distinct startup pulses */
    vibration_pulse(190);
    vTaskDelay(pdMS_TO_TICKS(100));
    vibration_pulse(250);
}

static void vibration_task(void *arg)
{
    (void)arg;

    vibration_cmd_t cmd;

    while (1) {
        if (xQueueReceive(
                vibration_queue,
                &cmd,
                portMAX_DELAY
            ) == pdTRUE) {

            switch (cmd) {
                case VIBRATION_CMD_CLICK:
                    if (vibration_enabled) {
                        play_click();
                    }
                    break;

                case VIBRATION_CMD_STARTUP:
                    if (vibration_enabled) {
                        play_startup();
                    }
                    break;

                default:
                    break;
            }
        }
    }
}

static void queue_vibration(vibration_cmd_t cmd)
{
    if (!vibration_queue) {
        return;
    }

    (void)xQueueSend(vibration_queue, &cmd, 0);
}

void vibration_set_enabled(int enabled)
{
    bool new_state = enabled != 0;

    if (new_state) {
        vibration_enabled = true;
        ESP_LOGI(TAG, "Vibration ON");
    } else {
        vibration_enabled = false;
        motor_off();

        if (vibration_queue) {
            xQueueReset(vibration_queue);
        }

        ESP_LOGI(TAG, "Vibration OFF");
    }
}

int vibration_is_enabled(void)
{
    return vibration_enabled ? 1 : 0;
}

void vibration_click(void)
{
    if (vibration_enabled) {
        queue_vibration(VIBRATION_CMD_CLICK);
    }
}

void vibration_startup(void)
{
    if (vibration_enabled) {
        queue_vibration(VIBRATION_CMD_STARTUP);
    }
}

/* ---------------- Settings checkbox ---------------- */

static void vibration_checkbox_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *checkbox = lv_event_get_target(e);

    bool checked =
        lv_obj_has_state(checkbox, LV_STATE_CHECKED);

    if (checked) {
        /*
         * ON:
         * enable vibration, then give one haptic confirmation.
         * Buzzer click also plays as UI feedback.
         */
        vibration_set_enabled(1);
        buzzer_click();
        vibration_click();
    } else {
        /*
         * OFF:
         * disable immediately.
         * No vibration pulse is played while turning vibration off.
         * Keep buzzer click as normal UI feedback.
         */
        buzzer_click();
        vibration_set_enabled(0);
    }
}

void vibration_checkbox_init(lv_obj_t *checkbox)
{
    if (!checkbox) {
        ESP_LOGE(TAG, "Vibration checkbox is NULL");
        return;
    }

    /* Always start enabled. */
    vibration_enabled = true;
    lv_obj_add_state(checkbox, LV_STATE_CHECKED);

    lv_obj_add_event_cb(
        checkbox,
        vibration_checkbox_cb,
        LV_EVENT_VALUE_CHANGED,
        NULL
    );

    ESP_LOGI(TAG, "Vibration checkbox ready: ON");
}

void vibration_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VIBRATION_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));

    vibration_ready = true;
    vibration_enabled = true;

    motor_off();

    vibration_queue =
        xQueueCreate(
            VIBRATION_QUEUE_LEN,
            sizeof(vibration_cmd_t)
        );

    if (!vibration_queue) {
        ESP_LOGE(TAG, "Failed to create vibration queue");
        return;
    }

    BaseType_t task_ok =
        xTaskCreate(
            vibration_task,
            "vibration",
            VIBRATION_TASK_STACK,
            NULL,
            VIBRATION_TASK_PRIO,
            NULL
        );

    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create vibration task");
        return;
    }

    ESP_LOGI(
        TAG,
        "GPIO vibration ready: SIG=GPIO%d, boot state=ON",
        VIBRATION_GPIO
    );
}