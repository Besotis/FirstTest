#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Passive buzzer on GPIO15 */
void buzzer_init(void);

/* Startup "cip cip" */
void buzzer_startup(void);

/* Short UI button click */
void buzzer_click(void);

/* Volume 0..100% */
void buzzer_set_volume(uint8_t percent);
uint8_t buzzer_get_volume(void);

/*
 * Connect SquareLine Volume slider directly to the buzzer:
 * - slider range: 0..100
 * - moving it changes buzzer volume immediately
 * - releasing it plays startup-style "cip cip" at the selected level
 */
void buzzer_volume_slider_init(lv_obj_t *slider);

#ifdef __cplusplus
}
#endif
