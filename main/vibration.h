#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Grobotronics vibration motor module:
 * VCC -> 5V
 * GND -> GND
 * SIG -> GPIO16
 *
 * This version uses simple digital GPIO control:
 * GPIO16 HIGH = vibration ON
 * GPIO16 LOW  = vibration OFF
 */
void vibration_init(void);

void vibration_click(void);
void vibration_startup(void);

void vibration_set_enabled(int enabled);
int vibration_is_enabled(void);

/*
 * Connect SquareLine checkbox to vibration ON/OFF.
 * Vibration starts enabled and the checkbox is forced CHECKED at boot.
 */
void vibration_checkbox_init(lv_obj_t *checkbox);

#ifdef __cplusplus
}
#endif
