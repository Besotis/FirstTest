#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"

#define MOSI 11
#define MISO 13
#define SCLK 12
#define LCD_CS 10
#define DC 7
#define RST 6
#define BL 5

#define W 128
#define H 160

/* Change ONLY these three values for testing. */
#define X_OFFSET 2
#define Y_OFFSET 1
#define USE_INVERSION 0   // 0 = INVOFF, 1 = INVON

static spi_device_handle_t spi;
static const char *TAG="LCD_ARTIFACT";

static void cmd(uint8_t c) {
    gpio_set_level(DC,0);
    spi_transaction_t t={.length=8,.tx_buffer=&c};
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi,&t));
}
static void data(const void *p,size_t n) {
    if(!n) return;
    gpio_set_level(DC,1);
    spi_transaction_t t={.length=n*8,.tx_buffer=p};
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi,&t));
}
static void d8(uint8_t v){data(&v,1);}

static void reset_lcd(void){
    gpio_set_level(RST,1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(RST,0); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(RST,1); vTaskDelay(pdMS_TO_TICKS(150));
}

static void init_lcd(void){
    reset_lcd();
    cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11); vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x3A); d8(0x05);              // RGB565
    cmd(0x36); d8(0x00);              // portrait
#if USE_INVERSION
    cmd(0x21);                         // INVON
#else
    cmd(0x20);                         // INVOFF
#endif
    cmd(0x13); vTaskDelay(pdMS_TO_TICKS(10));
    cmd(0x29); vTaskDelay(pdMS_TO_TICKS(120));
}

static void window(uint16_t x0,uint16_t y0,uint16_t x1,uint16_t y1){
    x0+=X_OFFSET; x1+=X_OFFSET;
    y0+=Y_OFFSET; y1+=Y_OFFSET;
    uint8_t b[4];
    cmd(0x2A);
    b[0]=x0>>8;b[1]=x0;b[2]=x1>>8;b[3]=x1;data(b,4);
    cmd(0x2B);
    b[0]=y0>>8;b[1]=y0;b[2]=y1>>8;b[3]=y1;data(b,4);
    cmd(0x2C);
}

static void fill_rect(int x,int y,int w,int h,uint16_t c){
    if(x<0||y<0||x+w>W||y+h>H||w<=0||h<=0)return;
    enum {N=128};
    uint8_t b[N*2];
    for(int i=0;i<N;i++){b[2*i]=c>>8;b[2*i+1]=c;}
    window(x,y,x+w-1,y+h-1);
    int left=w*h;
    while(left){int n=left>N?N:left;data(b,n*2);left-=n;}
}
static void fill(uint16_t c){fill_rect(0,0,W,H,c);}

static void test_pattern(void){
    fill(0x0000); // black
    /* 1px white border: makes offset/artifact errors obvious */
    fill_rect(0,0,W,1,0xFFFF);
    fill_rect(0,H-1,W,1,0xFFFF);
    fill_rect(0,0,1,H,0xFFFF);
    fill_rect(W-1,0,1,H,0xFFFF);

    /* colored corner blocks */
    fill_rect(1,1,12,12,0xF800);           // top-left red
    fill_rect(W-13,1,12,12,0x07E0);        // top-right green
    fill_rect(1,H-13,12,12,0x001F);        // bottom-left blue
    fill_rect(W-13,H-13,12,12,0xFFE0);     // bottom-right yellow
}

static void hw_init(void){
    gpio_config_t io={
        .pin_bit_mask=(1ULL<<DC)|(1ULL<<RST)|(1ULL<<BL),
        .mode=GPIO_MODE_OUTPUT
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(BL,0);

    spi_bus_config_t bus={
        .mosi_io_num=MOSI,.miso_io_num=MISO,.sclk_io_num=SCLK,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev={
        .clock_speed_hz=10*1000*1000,.mode=0,.spics_io_num=LCD_CS,.queue_size=1
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST,&dev,&spi));
}

void app_main(void){
    ESP_LOGI(TAG,"ESP32-S3 ST7735 artifact test");
    ESP_LOGI(TAG,"X_OFFSET=%d Y_OFFSET=%d INVERSION=%d",X_OFFSET,Y_OFFSET,USE_INVERSION);
    hw_init();
    init_lcd();
    gpio_set_level(BL,1);

    while(1){
        test_pattern();
        vTaskDelay(pdMS_TO_TICKS(3000));
        fill(0xF800); vTaskDelay(pdMS_TO_TICKS(1000));
        fill(0x07E0); vTaskDelay(pdMS_TO_TICKS(1000));
        fill(0x001F); vTaskDelay(pdMS_TO_TICKS(1000));
        fill(0xFFFF); vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
