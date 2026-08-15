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
#include "navigation.h"

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

    static lv_point_t last_point = {0, 0};
    static bool was_pressed = false;

    if (gpio_get_level(PIN_TOUCH_IRQ) == 0) {
        uint16_t raw_x = touch_read_avg(0xD0, 6);
        uint16_t raw_y = touch_read_avg(0x90, 6);

        last_point.x = map_clamped(raw_y,
                                   TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX,
                                   LCD_WIDTH - 1, 0);

        last_point.y = map_clamped(raw_x,
                                   TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX,
                                   LCD_HEIGHT - 1, 0);

        if (!was_pressed) {
            navigation_touch_press(last_point.x, last_point.y);
            was_pressed = true;
        } else {
            navigation_touch_move(last_point.x, last_point.y);
        }

        data->point = last_point;
        data->state = LV_INDEV_STATE_PR;
    } else {
        if (was_pressed) {
            navigation_touch_release(last_point.x, last_point.y);
            was_pressed = false;
        }

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

        navigation_process();

        /*
         * One FreeRTOS tick is required here. With CONFIG_FREERTOS_HZ=100,
         * a 5 ms delay can round to 0 ticks and starve IDLE0/watchdog.
         */
        vTaskDelay(1);
    }
}