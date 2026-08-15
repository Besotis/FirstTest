#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "ui.h"

/* ---------------- Hardware ---------------- */

#define PIN_MOSI        11
#define PIN_MISO        13
#define PIN_SCLK        12

#define PIN_LCD_CS      10
#define PIN_LCD_DC       7
#define PIN_LCD_RST      6
#define PIN_LCD_BL       5

#define PIN_TOUCH_CS     4
#define PIN_TOUCH_IRQ    8

#define LCD_WIDTH      160
#define LCD_HEIGHT     128

/* Confirmed for this ST7735S module */
#define LCD_X_OFFSET     1
#define LCD_Y_OFFSET     2   /* verify after 270 deg rotation */
#define LCD_INVERSION    0

/* Touch calibration from the working raw test.
   X is intentionally inverted. Fine calibration can be done later. */
#define TOUCH_RAW_X_MIN  300
#define TOUCH_RAW_X_MAX 3800
#define TOUCH_RAW_Y_MIN  300
#define TOUCH_RAW_Y_MAX 3800

/* LVGL partial draw buffer */
#define LVGL_BUF_LINES   20
#define LVGL_TICK_MS      5

static const char *TAG = "FIRSTTEST";

static spi_device_handle_t lcd_spi;
static spi_device_handle_t touch_spi;

static lv_disp_draw_buf_t lvgl_draw_buf;
static lv_color_t lvgl_buf1[LCD_WIDTH * LVGL_BUF_LINES];

static lv_disp_drv_t lvgl_disp_drv;
static lv_indev_drv_t lvgl_indev_drv;

#define TOUCH_MOVE_THRESHOLD 12

/* 50% of the 128 px display height = BACK threshold */
#define SWIPE_BACK_MIN_PIXELS 35

/* Visual slide limits / animation */
#define SWIPE_ANIM_TIME_MS 120

static bool touch_was_pressed = false;
static bool touch_moved = false;
static lv_point_t touch_press_start = {0, 0};
static lv_point_t touch_last_point = {0, 0};

/*
 * Touch read callback only records physical finger movement.
 * The main LVGL loop performs UI movement/animation.
 */
static bool drag_release_pending = false;
static int drag_release_dx = 0;
static int drag_release_dy = 0;

static lv_obj_t *drag_obj = NULL;
static int drag_translate_y = 0;
static bool nav_animation_running = false;
static bool nav_animation_back = false;
static lv_obj_t *nav_animation_obj = NULL;

/* ---------------- LCD low level ---------------- */

static void lcd_cmd(uint8_t value)
{
    gpio_set_level(PIN_LCD_DC, 0);

    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &value,
    };

    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
}

static void lcd_data(const void *data, size_t len)
{
    if (len == 0) {
        return;
    }

    gpio_set_level(PIN_LCD_DC, 1);

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };

    ESP_ERROR_CHECK(spi_device_polling_transmit(lcd_spi, &t));
}

static void lcd_data8(uint8_t value)
{
    lcd_data(&value, 1);
}

static void lcd_reset(void)
{
    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
}

static void lcd_init(void)
{
    lcd_reset();

    lcd_cmd(0x01);                     /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x11);                     /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x3A);                     /* COLMOD */
    lcd_data8(0x05);                   /* RGB565 */

    lcd_cmd(0x36);                     /* MADCTL */
    lcd_data8(0xA0);                   /* 270 deg landscape */

#if LCD_INVERSION
    lcd_cmd(0x21);                     /* INVON */
#else
    lcd_cmd(0x20);                     /* INVOFF */
#endif

    lcd_cmd(0x13);                     /* NORON */
    vTaskDelay(pdMS_TO_TICKS(10));

    lcd_cmd(0x29);                     /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void lcd_set_window(uint16_t x0, uint16_t y0,
                           uint16_t x1, uint16_t y1)
{
    x0 += LCD_X_OFFSET;
    x1 += LCD_X_OFFSET;
    y0 += LCD_Y_OFFSET;
    y1 += LCD_Y_OFFSET;

    uint8_t d[4];

    lcd_cmd(0x2A);
    d[0] = x0 >> 8;
    d[1] = x0 & 0xFF;
    d[2] = x1 >> 8;
    d[3] = x1 & 0xFF;
    lcd_data(d, 4);

    lcd_cmd(0x2B);
    d[0] = y0 >> 8;
    d[1] = y0 & 0xFF;
    d[2] = y1 >> 8;
    d[3] = y1 & 0xFF;
    lcd_data(d, 4);

    lcd_cmd(0x2C);
}

/* ---------------- Touch low level ---------------- */

static uint16_t touch_read_12bit(uint8_t command)
{
    uint8_t tx[3] = { command, 0x00, 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    ESP_ERROR_CHECK(spi_device_polling_transmit(touch_spi, &t));

    return (uint16_t)(((rx[1] << 8) | rx[2]) >> 3);
}

static uint16_t touch_read_avg(uint8_t command, int samples)
{
    uint32_t sum = 0;

    for (int i = 0; i < samples; i++) {
        sum += touch_read_12bit(command);
    }

    return (uint16_t)(sum / samples);
}

static int map_clamped(int value,
                       int in_min, int in_max,
                       int out_min, int out_max)
{
    if (value < in_min) value = in_min;
    if (value > in_max) value = in_max;

    return out_min +
           (value - in_min) * (out_max - out_min) /
           (in_max - in_min);
}

/* ---------------- LVGL callbacks ---------------- */

static void lvgl_flush_cb(lv_disp_drv_t *disp_drv,
                          const lv_area_t *area,
                          lv_color_t *color_p)
{
    int32_t width = area->x2 - area->x1 + 1;
    int32_t height = area->y2 - area->y1 + 1;
    size_t bytes = (size_t)width * (size_t)height * sizeof(lv_color_t);

    lcd_set_window((uint16_t)area->x1,
                   (uint16_t)area->y1,
                   (uint16_t)area->x2,
                   (uint16_t)area->y2);

    /* SquareLine requires LV_COLOR_16_SWAP=1, therefore the RGB565 bytes
       in LVGL's buffer are already in the order expected by ST7735S. */
    lcd_data(color_p, bytes);

    lv_disp_flush_ready(disp_drv);
}

static void lvgl_touch_read_cb(lv_indev_drv_t *indev_drv,
                               lv_indev_data_t *data)
{
    (void)indev_drv;

    static lv_point_t last_point = { 0, 0 };

    if (gpio_get_level(PIN_TOUCH_IRQ) == 0) {
        uint16_t raw_x = touch_read_avg(0xD0, 6);
        uint16_t raw_y = touch_read_avg(0x90, 6);

        last_point.x = map_clamped(raw_y,
                                   TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX,
                                   LCD_WIDTH - 1, 0);

        last_point.y = map_clamped(raw_x,
                                   TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX,
                                   LCD_HEIGHT - 1, 0);

        if (!touch_was_pressed) {
            /* New physical touch */
            touch_was_pressed = true;
            touch_moved = false;
            touch_press_start = last_point;
            touch_last_point = last_point;
            drag_release_pending = false;
        } else {
            int dx = last_point.x - touch_press_start.x;
            int dy = last_point.y - touch_press_start.y;

            touch_last_point = last_point;

            int adx = (dx < 0) ? -dx : dx;
            int ady = (dy < 0) ? -dy : dy;

            if (adx >= TOUCH_MOVE_THRESHOLD ||
                ady >= TOUCH_MOVE_THRESHOLD) {
                touch_moved = true;
            }
        }

        data->point = last_point;
        data->state = LV_INDEV_STATE_PR;
    } else {
        if (touch_was_pressed) {
            drag_release_dx = touch_last_point.x - touch_press_start.x;
            drag_release_dy = touch_last_point.y - touch_press_start.y;
            drag_release_pending = true;
        }

        touch_was_pressed = false;
        data->point = last_point;
        data->state = LV_INDEV_STATE_REL;
    }
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

/* ---------------- Hardware init ---------------- */

static void hardware_init(void)
{
    gpio_config_t output_cfg = {
        .pin_bit_mask =
            (1ULL << PIN_LCD_DC) |
            (1ULL << PIN_LCD_RST) |
            (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_cfg));

    gpio_set_level(PIN_LCD_BL, 0);
    gpio_set_level(PIN_LCD_RST, 1);
    gpio_set_level(PIN_LCD_DC, 0);

    gpio_config_t touch_irq_cfg = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&touch_irq_cfg));

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LVGL_BUF_LINES * sizeof(lv_color_t) + 16,
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO)
    );

    spi_device_interface_config_t lcd_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(
        spi_bus_add_device(SPI2_HOST, &lcd_cfg, &lcd_spi)
    );

    spi_device_interface_config_t touch_cfg = {
        .clock_speed_hz = 500 * 1000,
        .mode = 0,
        .spics_io_num = PIN_TOUCH_CS,
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(
        spi_bus_add_device(SPI2_HOST, &touch_cfg, &touch_spi)
    );
}

/* ---------------- LVGL init ---------------- */

static void lvgl_init_all(void)
{
    lv_init();

    lv_disp_draw_buf_init(
        &lvgl_draw_buf,
        lvgl_buf1,
        NULL,
        LCD_WIDTH * LVGL_BUF_LINES
    );

    lv_disp_drv_init(&lvgl_disp_drv);
    lvgl_disp_drv.hor_res = LCD_WIDTH;
    lvgl_disp_drv.ver_res = LCD_HEIGHT;
    lvgl_disp_drv.flush_cb = lvgl_flush_cb;
    lvgl_disp_drv.draw_buf = &lvgl_draw_buf;
    lv_disp_drv_register(&lvgl_disp_drv);

    lv_indev_drv_init(&lvgl_indev_drv);
    lvgl_indev_drv.type = LV_INDEV_TYPE_POINTER;
    lvgl_indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&lvgl_indev_drv);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(
        esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000)
    );
}


/* ---------------- Navigation ---------------- */

typedef enum {
    PAGE_MAIN = 0,
    PAGE_XLR_OPTIONS,
    PAGE_XLR_TEST1,
    PAGE_XLR_TEST2,
    PAGE_XLR_TEST3,
} app_page_t;

static app_page_t current_page = PAGE_MAIN;

static void hide_obj(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_obj(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_page(app_page_t page)
{
    /* Hide all pages first */
    hide_obj(ui_Main_Menu);
    hide_obj(ui_XLR_test_options_container);
    hide_obj(ui_XLR_test1);
    hide_obj(ui_XLR_test2);
    hide_obj(ui_XLR_test3);

    switch (page) {
        case PAGE_MAIN:
            show_obj(ui_Main_Menu);
            break;

        case PAGE_XLR_OPTIONS:
            show_obj(ui_XLR_test_options_container);
            break;

        case PAGE_XLR_TEST1:
            show_obj(ui_XLR_test1);
            break;

        case PAGE_XLR_TEST2:
            show_obj(ui_XLR_test2);
            break;

        case PAGE_XLR_TEST3:
            show_obj(ui_XLR_test3);
            break;

        default:
            show_obj(ui_Main_Menu);
            page = PAGE_MAIN;
            break;
    }

    current_page = page;
}

static bool navigation_click_allowed(void)
{
    if (nav_animation_running) {
        ESP_LOGI(TAG, "RELEASE ignored: navigation animation running");
        return false;
    }

    if (touch_moved) {
        ESP_LOGI(TAG, "RELEASE ignored: touch was a swipe");
        return false;
    }

    return true;
}

static void click_xlr_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (!navigation_click_allowed()) {
            return;
        }

        show_page(PAGE_XLR_OPTIONS);
    }
}

static void click_test1_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (!navigation_click_allowed()) {
            return;
        }

        show_page(PAGE_XLR_TEST1);
    }
}

static void click_test2_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (!navigation_click_allowed()) {
            return;
        }

        show_page(PAGE_XLR_TEST2);
    }
}

static void click_test3_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (!navigation_click_allowed()) {
            return;
        }

        show_page(PAGE_XLR_TEST3);
    }
}

static void go_back_one_level(void)
{
    switch (current_page) {
        case PAGE_XLR_OPTIONS:
            show_page(PAGE_MAIN);
            break;

        case PAGE_XLR_TEST1:
        case PAGE_XLR_TEST2:
        case PAGE_XLR_TEST3:
            show_page(PAGE_XLR_OPTIONS);
            break;

        case PAGE_MAIN:
        default:
            break;
    }
}

static lv_obj_t *get_active_drag_obj(void)
{
    switch (current_page) {
        case PAGE_XLR_OPTIONS:
            return ui_XLR_test_options_container;

        case PAGE_XLR_TEST1:
            return ui_XLR_test1;

        case PAGE_XLR_TEST2:
            return ui_XLR_test2;

        case PAGE_XLR_TEST3:
            return ui_XLR_test3;

        case PAGE_MAIN:
        default:
            return NULL;
    }
}

static void set_page_translate_y(lv_obj_t *obj, int y)
{
    if (!obj) {
        return;
    }

    lv_obj_set_style_translate_y(obj, y, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void nav_anim_exec_cb(void *var, int32_t value)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    set_page_translate_y(obj, (int)value);
    drag_translate_y = (int)value;
}

static void nav_anim_ready_cb(lv_anim_t *a)
{
    (void)a;

    if (nav_animation_obj) {
        /*
         * Restore the old page to its original SquareLine position before
         * it is shown again later.
         */
        set_page_translate_y(nav_animation_obj, 0);
    }

    drag_translate_y = 0;
    drag_obj = NULL;

    if (nav_animation_back) {
        go_back_one_level();
        ESP_LOGI(TAG, "SLIDE -> BACK, page=%d", (int)current_page);
    }

    nav_animation_back = false;
    nav_animation_obj = NULL;
    nav_animation_running = false;
}

static void start_page_animation(lv_obj_t *obj, int from_y, int to_y, bool do_back)
{
    if (!obj) {
        if (do_back) {
            go_back_one_level();
        }
        return;
    }

    nav_animation_running = true;
    nav_animation_back = do_back;
    nav_animation_obj = obj;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, nav_anim_exec_cb);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, SWIPE_ANIM_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, nav_anim_ready_cb);
    lv_anim_start(&a);
}

static void update_page_drag(void)
{
    if (nav_animation_running || current_page == PAGE_MAIN) {
        return;
    }

    if (!touch_was_pressed || !touch_moved) {
        return;
    }

    int dx = touch_last_point.x - touch_press_start.x;
    int dy = touch_last_point.y - touch_press_start.y;

    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;

    /*
     * Drag only for primarily vertical upward movement.
     */
    if (dy < 0 && ady > adx) {
        if (!drag_obj) {
            drag_obj = get_active_drag_obj();
        }

        if (drag_obj) {
            if (dy < -LCD_HEIGHT) {
                dy = -LCD_HEIGHT;
            }

            drag_translate_y = dy;
            set_page_translate_y(drag_obj, drag_translate_y);
        }
    }
}

static void finish_page_drag(void)
{
    if (!drag_release_pending) {
        return;
    }

    drag_release_pending = false;

    if (current_page == PAGE_MAIN || !touch_moved) {
        drag_obj = NULL;
        drag_translate_y = 0;
        return;
    }

    int dx = drag_release_dx;
    int dy = drag_release_dy;

    int adx = (dx < 0) ? -dx : dx;
    int upward = (dy < 0) ? -dy : 0;

    bool vertical_up = (dy < 0) && (upward > adx);

    /*
     * Dynamic BACK threshold:
     * require 80% of the available distance from the touch start point
     * to the top of the screen.
     */
    int available_distance = touch_press_start.y;
    int required_distance = (available_distance * 80) / 100;

    if (required_distance < SWIPE_BACK_MIN_PIXELS) {
        required_distance = SWIPE_BACK_MIN_PIXELS;
    }

    bool pass_threshold =
        vertical_up &&
        (upward >= required_distance);

    if (!drag_obj) {
        drag_obj = get_active_drag_obj();
    }

    if (pass_threshold) {
        ESP_LOGI(TAG,
                 "SLIDE release -> BACK: startY=%d dx=%d dy=%d required=%d",
                 touch_press_start.y, dx, dy, required_distance);

        start_page_animation(drag_obj,
                             drag_translate_y,
                             -LCD_HEIGHT,
                             true);
    } else {
        ESP_LOGI(TAG,
                 "SLIDE release -> CANCEL: startY=%d dx=%d dy=%d required=%d",
                 touch_press_start.y, dx, dy, required_distance);

        if (drag_obj && drag_translate_y != 0) {
            start_page_animation(drag_obj,
                                 drag_translate_y,
                                 0,
                                 false);
        } else {
            drag_obj = NULL;
            drag_translate_y = 0;
        }
    }
}

static void navigation_init(void)
{
    /* Click navigation */
    lv_obj_add_event_cb(ui_XLR_menu_button,
                        click_xlr_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_XLR_test1_button,
                        click_test1_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_XLR_test2_button,
                        click_test2_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_XLR_test3_button,
                        click_test3_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    /* Known initial state */
    show_page(PAGE_MAIN);
}

/* ---------------- Main ---------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 + ST7735S + Touch + LVGL 8.3.11");
    ESP_LOGI(TAG, "LCD %dx%d, offset X=%d Y=%d",
             LCD_WIDTH, LCD_HEIGHT, LCD_X_OFFSET, LCD_Y_OFFSET);

    hardware_init();
    lcd_init();

    lvgl_init_all();
    ui_init();
    navigation_init();

    gpio_set_level(PIN_LCD_BL, 1);

    ESP_LOGI(TAG, "SquareLine UI started");

    while (1) {
        lv_timer_handler();

        /*
         * While the finger is moving upward, translate the active page with
         * the finger. On release either complete BACK or animate back to 0.
         */
        update_page_drag();
        finish_page_drag();

        /*
         * One FreeRTOS tick is required here. With CONFIG_FREERTOS_HZ=100,
         * pdMS_TO_TICKS(5) becomes 0 and starves IDLE0/watchdog.
         */
        vTaskDelay(1);
    }
}