#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void brightness_init(int gpio_num, lv_obj_t *slider);
void brightness_set(uint8_t percent);
uint8_t brightness_get(void);

#ifdef __cplusplus
}
#endif
