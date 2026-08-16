#include "xlr_test.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "buzzer.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "ui.h"

/* ---------------- XLR Normal Mode GPIOs ---------------- */

#define XLR_A1_GPIO    GPIO_NUM_40
#define XLR_A2_GPIO    GPIO_NUM_41
#define XLR_A3_GPIO    GPIO_NUM_42

#define XLR_B1_GPIO    GPIO_NUM_47
#define XLR_B2_GPIO    GPIO_NUM_48
#define XLR_B3_GPIO    GPIO_NUM_21

#define XLR_PIN_COUNT              6
#define XLR_LINE_COUNT             3

/* 256 complete matrix scans / second */
#define XLR_SCAN_RATE_HZ           256
#define XLR_SCAN_PERIOD_US         (1000000 / XLR_SCAN_RATE_HZ)

/* 128 scans = ~0.5 s */
#define XLR_SCANS_PER_UI_CYCLE     128

/* 10 perfect 0.5 s cycles = 5 seconds = 100% */
#define XLR_QUALITY_CYCLES         10

/*
 * Small settling delay after changing the source GPIO to OUTPUT HIGH.
 * A complete scan still finishes far below the 3.906 ms scan period.
 */
#define XLR_SETTLE_US              25

#define XLR_TASK_STACK             4096
#define XLR_TASK_PRIORITY          5

#define RESULT_COLOR_GOOD          0x72B540
#define RESULT_COLOR_NC            0xFFC800
#define RESULT_COLOR_FAULT         0xFF0032

static const char *TAG = "XLR_NORMAL";

enum {
    IDX_A1 = 0,
    IDX_A2,
    IDX_A3,
    IDX_B1,
    IDX_B2,
    IDX_B3,
};

static const gpio_num_t xlr_gpio[XLR_PIN_COUNT] = {
    XLR_A1_GPIO,
    XLR_A2_GPIO,
    XLR_A3_GPIO,
    XLR_B1_GPIO,
    XLR_B2_GPIO,
    XLR_B3_GPIO,
};

typedef enum {
    PIN_RESULT_GOOD = 0,
    PIN_RESULT_NC,
    PIN_RESULT_FAULT,
} pin_result_type_t;

typedef struct {
    pin_result_type_t type;
    uint8_t fault_mask; /* bits 0..2 = X1..X3 */
} pin_result_t;

typedef struct {
    bool saw_nc;
    uint8_t fault_mask;
} cycle_pin_accum_t;

typedef struct {
    pin_result_t pin[XLR_LINE_COUNT];
    uint8_t cable_quality;
    bool cycle_had_error;
    uint32_t cycle_number;
} xlr_ui_result_t;

static TaskHandle_t scan_task_handle = NULL;
static esp_timer_handle_t scan_timer = NULL;
static QueueHandle_t ui_result_queue = NULL;

static volatile bool test_running = false;

static cycle_pin_accum_t cycle_accum[XLR_LINE_COUNT];
static uint16_t scans_in_cycle = 0;
static uint8_t good_cycles = 0;
static uint32_t completed_cycles = 0;

/*
 * Smooth Cable Quality animation.
 *
 * Logical quality still advances by 10% every perfect 0.5 s cycle,
 * but the visible bar moves continuously across the whole 0.5 s window.
 */
static int cable_quality_display = 0;
static int cable_quality_anim_from = 0;
static int cable_quality_anim_to = 0;
static int64_t cable_quality_anim_start_us = 0;

#define CABLE_QUALITY_ANIM_US 500000

/* ---------------- GPIO scan helpers ---------------- */

static void set_pin_input_pulldown(gpio_num_t gpio)
{
    /*
     * Put output latch LOW first so there is no surprise HIGH pulse the next
     * time this GPIO becomes an output.
     */
    gpio_set_level(gpio, 0);

    ESP_ERROR_CHECK(gpio_set_direction(gpio, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_pullup_dis(gpio));
    ESP_ERROR_CHECK(gpio_pulldown_en(gpio));
}

static void all_pins_input_pulldown(void)
{
    for (int i = 0; i < XLR_PIN_COUNT; i++) {
        set_pin_input_pulldown(xlr_gpio[i]);
    }
}

static void drive_source_high(int source_index)
{
    gpio_num_t gpio = xlr_gpio[source_index];

    /*
     * Program the output latch before enabling output mode.
     * Every other test pin remains INPUT + PULLDOWN.
     */
    gpio_set_level(gpio, 1);
    ESP_ERROR_CHECK(gpio_set_direction(gpio, GPIO_MODE_OUTPUT));
}

static bool read_pin_stable(gpio_num_t gpio)
{
    /*
     * Three quick reads; 2/3 majority rejects a single noisy sample.
     */
    int high_count = 0;

    high_count += gpio_get_level(gpio) ? 1 : 0;
    esp_rom_delay_us(3);
    high_count += gpio_get_level(gpio) ? 1 : 0;
    esp_rom_delay_us(3);
    high_count += gpio_get_level(gpio) ? 1 : 0;

    return high_count >= 2;
}

static void scan_matrix(bool matrix[XLR_PIN_COUNT][XLR_PIN_COUNT])
{
    memset(matrix, 0, sizeof(bool) * XLR_PIN_COUNT * XLR_PIN_COUNT);

    for (int source = 0; source < XLR_PIN_COUNT; source++) {
        /*
         * Start every row from the same known electrical state.
         */
        all_pins_input_pulldown();

        drive_source_high(source);
        esp_rom_delay_us(XLR_SETTLE_US);

        for (int target = 0; target < XLR_PIN_COUNT; target++) {
            if (target == source) {
                continue;
            }

            matrix[source][target] =
                read_pin_stable(xlr_gpio[target]);
        }

        /*
         * Remove the drive before moving on to the next matrix row.
         */
        gpio_set_level(xlr_gpio[source], 0);
        set_pin_input_pulldown(xlr_gpio[source]);
    }

    all_pins_input_pulldown();
}

static bool confirmed_connection(
    const bool matrix[XLR_PIN_COUNT][XLR_PIN_COUNT],
    int a,
    int b)
{
    /*
     * A passive cable connection must be visible in BOTH scan directions.
     * This is exactly the user's requirement:
     * A1 reaches B1 AND B1 reaches A1.
     */
    return matrix[a][b] && matrix[b][a];
}

/* ---------------- Matrix analysis ---------------- */

static pin_result_t analyze_line(
    const bool matrix[XLR_PIN_COUNT][XLR_PIN_COUNT],
    int line_index)
{
    const int a_idx = IDX_A1 + line_index;
    const int b_idx = IDX_B1 + line_index;

    uint8_t opposite_b_mask = 0;
    uint8_t same_a_mask = 0;
    uint8_t same_b_mask = 0;

    /*
     * Which B-side numbered pins are reachable from this A-side pin?
     *
     * Examples:
     * A1 <-> B1          => bit X1
     * A1 <-> B2          => bit X2
     * A1 <-> B1 and B2   => bits X1,X2
     */
    for (int j = 0; j < XLR_LINE_COUNT; j++) {
        if (confirmed_connection(matrix, a_idx, IDX_B1 + j)) {
            opposite_b_mask |= (uint8_t)(1U << j);
        }
    }

    /*
     * Also detect shorts that exist on only one connector side.
     * With a normally wired cable these will usually propagate through to
     * the other side, but checking both connector faces makes the matrix
     * interpretation more complete.
     */
    for (int j = 0; j < XLR_LINE_COUNT; j++) {
        if (j == line_index) {
            continue;
        }

        if (confirmed_connection(matrix, a_idx, IDX_A1 + j)) {
            same_a_mask |= (uint8_t)(1U << j);
        }

        if (confirmed_connection(matrix, b_idx, IDX_B1 + j)) {
            same_b_mask |= (uint8_t)(1U << j);
        }
    }

    const uint8_t expected_mask = (uint8_t)(1U << line_index);

    /*
     * Perfect line:
     * - A[n] reaches only B[n]
     * - B[n] reaches A[n] (enforced by confirmed_connection)
     * - no same-side shorts
     */
    if (opposite_b_mask == expected_mask &&
        same_a_mask == 0 &&
        same_b_mask == 0) {

        pin_result_t result = {
            .type = PIN_RESULT_GOOD,
            .fault_mask = 0,
        };

        return result;
    }

    /*
     * Completely isolated line is N/C.
     */
    if (opposite_b_mask == 0 &&
        same_a_mask == 0 &&
        same_b_mask == 0) {

        pin_result_t result = {
            .type = PIN_RESULT_NC,
            .fault_mask = 0,
        };

        return result;
    }

    /*
     * Fault / cross / short.
     *
     * Swapped example:
     *   A1 <-> B2
     *   opposite_b_mask = X2
     *   result = -->X2
     *
     * Short example:
     *   A1 reaches B1 and B2
     *   result = -->X1,X2
     *
     * If the short exists only on one connector face, include the nominal
     * line itself plus the same-side lines in the displayed mask.
     */
    uint8_t fault_mask = opposite_b_mask;

    if (same_a_mask || same_b_mask) {
        fault_mask |= expected_mask;
        fault_mask |= same_a_mask;
        fault_mask |= same_b_mask;
    }

    /*
     * Defensive fallback: any non-N/C fault should have something to show.
     */
    if (fault_mask == 0) {
        fault_mask = expected_mask;
    }

    pin_result_t result = {
        .type = PIN_RESULT_FAULT,
        .fault_mask = fault_mask,
    };

    return result;
}

static void reset_cycle_accumulator(void)
{
    memset(cycle_accum, 0, sizeof(cycle_accum));
    scans_in_cycle = 0;
}

static void accumulate_scan(
    const bool matrix[XLR_PIN_COUNT][XLR_PIN_COUNT])
{
    for (int line = 0; line < XLR_LINE_COUNT; line++) {
        pin_result_t result = analyze_line(matrix, line);

        if (result.type == PIN_RESULT_NC) {
            cycle_accum[line].saw_nc = true;
        } else if (result.type == PIN_RESULT_FAULT) {
            /*
             * If several different X faults happen while the cable is being
             * moved, show their union for this 0.5 s result window.
             */
            cycle_accum[line].fault_mask |= result.fault_mask;
        }
    }

    scans_in_cycle++;
}

static pin_result_t finish_cycle_line(int line)
{
    if (cycle_accum[line].fault_mask != 0) {
        pin_result_t result = {
            .type = PIN_RESULT_FAULT,
            .fault_mask = cycle_accum[line].fault_mask,
        };
        return result;
    }

    if (cycle_accum[line].saw_nc) {
        pin_result_t result = {
            .type = PIN_RESULT_NC,
            .fault_mask = 0,
        };
        return result;
    }

    pin_result_t result = {
        .type = PIN_RESULT_GOOD,
        .fault_mask = 0,
    };
    return result;
}

static void publish_completed_cycle(void)
{
    xlr_ui_result_t ui = {0};

    bool all_good = true;

    for (int line = 0; line < XLR_LINE_COUNT; line++) {
        ui.pin[line] = finish_cycle_line(line);

        if (ui.pin[line].type != PIN_RESULT_GOOD) {
            all_good = false;
        }
    }

    ui.cycle_had_error = !all_good;

    if (all_good) {
        if (good_cycles < XLR_QUALITY_CYCLES) {
            good_cycles++;
        }
    } else {
        /*
         * Any single bad matrix out of the ~128 scans in this 0.5 s window
         * resets Cable Quality immediately.
         */
        good_cycles = 0;
    }

    ui.cable_quality =
        (uint8_t)(good_cycles * (100 / XLR_QUALITY_CYCLES));

    cable_quality_anim_from = cable_quality_display;
    cable_quality_anim_to = ui.cable_quality;
    cable_quality_anim_start_us = esp_timer_get_time();

    completed_cycles++;
    ui.cycle_number = completed_cycles;

    /*
     * Queue length is one: if LVGL is briefly busy, keep only the newest
     * 0.5 s result. Scanner itself never waits for UI.
     */
    xQueueOverwrite(ui_result_queue, &ui);

    reset_cycle_accumulator();
}

/* ---------------- 256 Hz scheduler ---------------- */

static void scan_timer_cb(void *arg)
{
    (void)arg;

    if (test_running && scan_task_handle) {
        xTaskNotifyGive(scan_task_handle);
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    bool matrix[XLR_PIN_COUNT][XLR_PIN_COUNT];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!test_running) {
            continue;
        }

        scan_matrix(matrix);

        if (!test_running) {
            continue;
        }

        accumulate_scan(matrix);

        if (scans_in_cycle >= XLR_SCANS_PER_UI_CYCLE) {
            publish_completed_cycle();
        }
    }
}

/* ---------------- LVGL result rendering ---------------- */

static void format_fault_text(
    uint8_t mask,
    char *buffer,
    size_t buffer_size)
{
    size_t used = 0;

    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        "-->"
    );

    bool first = true;

    for (int i = 0; i < XLR_LINE_COUNT; i++) {
        if (!(mask & (1U << i))) {
            continue;
        }

        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%sX%d",
            first ? "" : ",",
            i + 1
        );

        first = false;

        if (used >= buffer_size) {
            break;
        }
    }
}

static void render_pin_result(lv_obj_t *label, pin_result_t result)
{
    char text[24];

    switch (result.type) {
        case PIN_RESULT_GOOD:
            lv_label_set_text(label, "GOOD");
            lv_obj_set_style_text_color(
                label,
                lv_color_hex(RESULT_COLOR_GOOD),
                LV_PART_MAIN | LV_STATE_DEFAULT
            );
            break;

        case PIN_RESULT_NC:
            lv_label_set_text(label, "N/C");
            lv_obj_set_style_text_color(
                label,
                lv_color_hex(RESULT_COLOR_NC),
                LV_PART_MAIN | LV_STATE_DEFAULT
            );
            break;

        case PIN_RESULT_FAULT:
        default:
            format_fault_text(
                result.fault_mask,
                text,
                sizeof(text)
            );

            lv_label_set_text(label, text);
            lv_obj_set_style_text_color(
                label,
                lv_color_hex(RESULT_COLOR_FAULT),
                LV_PART_MAIN | LV_STATE_DEFAULT
            );
            break;
    }

    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- Public API ---------------- */

void xlr_test_init(void)
{
    /*
     * All six cable-test nodes idle as INPUT + PULLDOWN.
     */
    all_pins_input_pulldown();

    ui_result_queue =
        xQueueCreate(1, sizeof(xlr_ui_result_t));

    if (!ui_result_queue) {
        ESP_LOGE(TAG, "Failed to create UI result queue");
        return;
    }

    BaseType_t task_ok = xTaskCreate(
        scan_task,
        "xlr_scan",
        XLR_TASK_STACK,
        NULL,
        XLR_TASK_PRIORITY,
        &scan_task_handle
    );

    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create XLR scan task");
        scan_task_handle = NULL;
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = scan_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "xlr_256hz",
        .skip_unhandled_events = true,
    };

    ESP_ERROR_CHECK(
        esp_timer_create(&timer_args, &scan_timer)
    );

    test_running = false;
    reset_cycle_accumulator();

    ESP_LOGI(
        TAG,
        "Ready: A1=40 A2=41 A3=42 B1=47 B2=48 B3=21, scan=%d/s",
        XLR_SCAN_RATE_HZ
    );
}

void xlr_test_start(void)
{
    if (test_running || !scan_timer || !scan_task_handle) {
        return;
    }

    all_pins_input_pulldown();

    good_cycles = 0;
    completed_cycles = 0;

    cable_quality_display = 0;
    cable_quality_anim_from = 0;
    cable_quality_anim_to = 0;
    cable_quality_anim_start_us = esp_timer_get_time();

    reset_cycle_accumulator();

    if (ui_result_queue) {
        xQueueReset(ui_result_queue);
    }

    /*
     * Initial UI state. This function is called from navigation/LVGL task.
     */
    lv_obj_clear_flag(ui_Pin1Result, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_Pin2Result, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_Pin3Result, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ui_Pin1Result, "...");
    lv_label_set_text(ui_Pin2Result, "...");
    lv_label_set_text(ui_Pin3Result, "...");

    lv_obj_set_style_text_color(
        ui_Pin1Result, lv_color_hex(0x888888),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );
    lv_obj_set_style_text_color(
        ui_Pin2Result, lv_color_hex(0x888888),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );
    lv_obj_set_style_text_color(
        ui_Pin3Result, lv_color_hex(0x888888),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_bar_set_range(ui_Cable_Quality, 0, 100);
    lv_bar_set_value(ui_Cable_Quality, 0, LV_ANIM_OFF);

    /*
     * Reuse the existing SquareLine testing-mode label.
     * Keep text short so it doesn't visually collide with other UI objects.
     */
    lv_label_set_text(
        ui_XLR_testing_mode_lable,
        "NORMAL"
    );

    test_running = true;

    ESP_ERROR_CHECK(
        esp_timer_start_periodic(
            scan_timer,
            XLR_SCAN_PERIOD_US
        )
    );

    ESP_LOGI(TAG, "XLR Normal test START");
}

void xlr_test_stop(void)
{
    if (!test_running) {
        return;
    }

    test_running = false;

    if (scan_timer && esp_timer_is_active(scan_timer)) {
        ESP_ERROR_CHECK(esp_timer_stop(scan_timer));
    }

    all_pins_input_pulldown();

    if (ui_result_queue) {
        xQueueReset(ui_result_queue);
    }

    ESP_LOGI(TAG, "XLR Normal test STOP");
}

int xlr_test_is_running(void)
{
    return test_running ? 1 : 0;
}

void xlr_test_process(void)
{
    if (!ui_result_queue) {
        return;
    }

    xlr_ui_result_t ui;

    /*
     * Apply all pending data in the LVGL/main task only.
     */
    while (xQueueReceive(ui_result_queue, &ui, 0) == pdTRUE) {
        if (!test_running) {
            continue;
        }

        render_pin_result(ui_Pin1Result, ui.pin[0]);
        render_pin_result(ui_Pin2Result, ui.pin[1]);
        render_pin_result(ui_Pin3Result, ui.pin[2]);

        /*
         * The scan task has already scheduled the new visual target.
         * Do not jump the bar here.
         */

        /*
         * Exactly one high 4000 Hz warning beep for every bad 0.5 s cycle.
         * If the error remains, this repeats once every cycle.
         */
        if (ui.cycle_had_error) {
            buzzer_warning();
        }

        ESP_LOGI(
            TAG,
            "cycle=%lu quality=%u%% %s",
            (unsigned long)ui.cycle_number,
            (unsigned)ui.cable_quality,
            ui.cycle_had_error ? "FAULT" : "GOOD"
        );
    }

    /*
     * Time-based interpolation over the entire 500 ms cycle.
     *
     * For a good cable the bar flows continuously:
     * 0->10, then immediately 10->20, then 20->30, ...
     * with no pause at the 10% boundaries.
     */
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - cable_quality_anim_start_us;

    if (elapsed_us < 0) {
        elapsed_us = 0;
    }

    if (elapsed_us >= CABLE_QUALITY_ANIM_US) {
        cable_quality_display = cable_quality_anim_to;
    } else {
        int delta =
            cable_quality_anim_to - cable_quality_anim_from;

        cable_quality_display =
            cable_quality_anim_from +
            (int)((delta * elapsed_us) / CABLE_QUALITY_ANIM_US);
    }

    lv_bar_set_value(
        ui_Cable_Quality,
        cable_quality_display,
        LV_ANIM_OFF
    );

}