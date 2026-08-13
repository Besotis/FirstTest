# ST7735S artifact/offset test — ESP32-S3

Target: ESP32-S3.

Wiring remains:
- MOSI GPIO11
- MISO GPIO13
- SCLK GPIO12
- LCD CS GPIO10
- DC GPIO7
- RESET GPIO6
- BL GPIO5

At the top of main/main.c change:

    #define X_OFFSET 0
    #define Y_OFFSET 0
    #define USE_INVERSION 0

Try offsets such as:
0,0
1,0
2,0
0,1
0,2
1,2
2,1

Then try USE_INVERSION 1.

The test draws a 1-pixel white border plus colored corner blocks, then full-screen
red/green/blue/white. This makes unused rows/columns and incorrect offsets easy to see.

Build:
    idf.py set-target esp32s3
    idf.py build
    idf.py -p /dev/cu.usbmodemYOURPORT flash monitor
