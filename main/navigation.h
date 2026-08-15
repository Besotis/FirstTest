#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UI navigation:
 *
 * Main Menu
 *   ├─ XLR      -> XLR test page
 *   ├─ RJ45     -> RJ45 test page
 *   └─ Settings -> Settings page
 *
 * XLR/RJ45:
 *   swipe UP -> animated BACK to Main Menu
 *
 * Settings:
 *   uses its dedicated BACK button.
 *   Swipe-up is intentionally NOT treated as BACK because Settings is
 *   vertically scrollable.
 */

void navigation_init(void);

/* Called by the touch driver in main.c */
void navigation_touch_press(int16_t x, int16_t y);
void navigation_touch_move(int16_t x, int16_t y);
void navigation_touch_release(int16_t x, int16_t y);

/* Call once from the LVGL main loop after lv_timer_handler(). */
void navigation_process(void);

#ifdef __cplusplus
}
#endif
