#include "navigation.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"

#define LCD_HEIGHT               128

#define TOUCH_MOVE_THRESHOLD      12
#define SWIPE_BACK_MIN_PIXELS     35
#define SWIPE_BACK_PERCENT        80
#define SWIPE_ANIM_TIME_MS       120

static const char *TAG = "NAV";

/*
 * New SquareLine structure in FirstTest-main(5):
 *
 * ui_Main_Menu
 * ui_XLR_test
 * ui_RJ45_test
 * ui_Settings
 *
 * Settings test-mode controls now live inside ui_Settings, so there is no
 * separate XLR mode/menu page in navigation anymore.
 */
typedef enum {
    PAGE_MAIN = 0,
    PAGE_XLR,
    PAGE_RJ45,
    PAGE_SETTINGS,
} app_page_t;

static app_page_t current_page = PAGE_MAIN;

/* Physical touch state */
static bool touch_pressed = false;
static bool touch_moved = false;
static lv_point_t touch_start = {0, 0};
static lv_point_t touch_last = {0, 0};

/* Release is consumed in navigation_process(), after LVGL processed RELEASED. */
static bool release_pending = false;
static int release_dx = 0;
static int release_dy = 0;

/* Visual slide animation state */
static lv_obj_t *drag_obj = NULL;
static int drag_translate_y = 0;

static bool animation_running = false;
static bool animation_back = false;
static lv_obj_t *animation_obj = NULL;

/* Settings custom scroll thumb */
static bool settings_thumb_dragging = false;
static int16_t settings_thumb_press_y = 0;
static int32_t settings_thumb_start_y = 0;

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

static void reset_translate(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_translate_y(obj, 0,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void show_page(app_page_t page)
{
    /*
     * Main Menu, XLR and RJ45 are children of ui_Main_container.
     * Settings is a separate child of ui_Screen1.
     */
    hide_obj(ui_Main_Menu);
    hide_obj(ui_XLR_test);
    hide_obj(ui_RJ45_test);
    hide_obj(ui_Settings);

    /* Always restore pages before showing them again. */
    reset_translate(ui_XLR_test);
    reset_translate(ui_RJ45_test);

    switch (page) {
        case PAGE_MAIN:
            show_obj(ui_Main_Menu);
            break;

        case PAGE_XLR:
            show_obj(ui_XLR_test);
            break;

        case PAGE_RJ45:
            show_obj(ui_RJ45_test);
            break;

        case PAGE_SETTINGS:
            show_obj(ui_Settings);
            break;

        default:
            page = PAGE_MAIN;
            show_obj(ui_Main_Menu);
            break;
    }

    current_page = page;
    ESP_LOGI(TAG, "page=%d", (int)current_page);
}

static bool tap_allowed(void)
{
    if (animation_running) {
        return false;
    }

    /*
     * If this physical touch moved, RELEASED must not become a menu click.
     * touch_moved is reset only on the next physical press.
     */
    if (touch_moved) {
        ESP_LOGI(TAG, "RELEASE ignored: touch was a drag/swipe");
        return false;
    }

    return true;
}


/* ---------------- Settings scroll thumb ---------------- */

static int32_t settings_get_max_scroll(void)
{
    /*
     * Current scroll position + remaining scroll below = total scroll range.
     * This remains valid while the Settings page is already scrolled.
     */
    int32_t current = lv_obj_get_scroll_y(ui_Settings_menu_scrollable);
    int32_t bottom = lv_obj_get_scroll_bottom(ui_Settings_menu_scrollable);
    int32_t max_scroll = current + bottom;

    return max_scroll > 0 ? max_scroll : 0;
}

static int32_t settings_get_thumb_travel(void)
{
    lv_obj_update_layout(ui_Settings);

    int32_t track_h = lv_obj_get_height(ui_ScrollTrack);
    int32_t thumb_h = lv_obj_get_height(ui_ScrollThumb);
    int32_t travel = track_h - thumb_h;

    return travel > 0 ? travel : 0;
}

static void settings_set_thumb_y(int32_t y)
{
    int32_t travel = settings_get_thumb_travel();

    if (y < 0) {
        y = 0;
    }
    if (y > travel) {
        y = travel;
    }

    /*
     * ScrollThumb is TOP_MID aligned in SquareLine, so translate_y=0 is
     * exactly the visual top of the ScrollTrack.
     */
    lv_obj_set_style_translate_y(ui_ScrollThumb,
                                 y,
                                 LV_PART_MAIN | LV_STATE_DEFAULT);
}

static int32_t settings_get_thumb_y(void)
{
    return lv_obj_get_style_translate_y(ui_ScrollThumb, LV_PART_MAIN);
}

static void settings_update_thumb_from_scroll(void)
{
    if (settings_thumb_dragging) {
        return;
    }

    int32_t max_scroll = settings_get_max_scroll();
    int32_t travel = settings_get_thumb_travel();
    int32_t scroll_y = lv_obj_get_scroll_y(ui_Settings_menu_scrollable);

    if (scroll_y < 0) {
        scroll_y = 0;
    }
    if (scroll_y > max_scroll) {
        scroll_y = max_scroll;
    }

    int32_t thumb_y = 0;

    if (max_scroll > 0 && travel > 0) {
        thumb_y = (scroll_y * travel) / max_scroll;
    }

    settings_set_thumb_y(thumb_y);
}

static void settings_reset_to_top(void)
{
    settings_thumb_dragging = false;

    /*
     * Force SquareLine's flex layout to be calculated while Settings is
     * visible, then reset the scroll position.
     */
    lv_obj_update_layout(ui_Settings);
    lv_obj_update_layout(ui_Settings_menu_scrollable);

    lv_obj_scroll_to_y(ui_Settings_menu_scrollable, 0, LV_ANIM_OFF);

    /*
     * Run layout once more because scrolling/flex can update coordinates.
     */
    lv_obj_update_layout(ui_Settings_menu_scrollable);

    settings_set_thumb_y(0);

    ESP_LOGI(TAG,
             "Settings reset: scroll_y=%ld max=%ld thumb=%ld",
             (long)lv_obj_get_scroll_y(ui_Settings_menu_scrollable),
             (long)settings_get_max_scroll(),
             (long)settings_get_thumb_y());
}

static void settings_scroll_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_SCROLL) {
        settings_update_thumb_from_scroll();
    }
}

static void settings_thumb_pressed_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    settings_thumb_dragging = true;
    settings_thumb_press_y = p.y;
    settings_thumb_start_y = settings_get_thumb_y();
}

static void settings_thumb_pressing_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSING ||
        !settings_thumb_dragging) {
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int32_t travel = settings_get_thumb_travel();
    int32_t new_thumb_y =
        settings_thumb_start_y + (p.y - settings_thumb_press_y);

    if (new_thumb_y < 0) {
        new_thumb_y = 0;
    }
    if (new_thumb_y > travel) {
        new_thumb_y = travel;
    }

    settings_set_thumb_y(new_thumb_y);

    int32_t max_scroll = settings_get_max_scroll();
    int32_t target_scroll = 0;

    if (travel > 0 && max_scroll > 0) {
        target_scroll = (new_thumb_y * max_scroll) / travel;
    }

    /*
     * The thumb is the controller while dragging, therefore scroll without
     * animation so content follows the finger immediately.
     */
    lv_obj_scroll_to_y(ui_Settings_menu_scrollable,
                       target_scroll,
                       LV_ANIM_OFF);
}

static void settings_thumb_released_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED ||
        code == LV_EVENT_PRESS_LOST) {
        settings_thumb_dragging = false;
        settings_update_thumb_from_scroll();
    }
}


/* ---------------- Button callbacks ---------------- */

static void xlr_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && tap_allowed()) {
        show_page(PAGE_XLR);
    }
}

static void rj45_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && tap_allowed()) {
        show_page(PAGE_RJ45);
    }
}

static void settings_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && tap_allowed()) {
        show_page(PAGE_SETTINGS);
    }
}

static void settings_back_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && tap_allowed()) {
        /*
         * Hide Settings first, then reset its scroll position while it is
         * invisible. Next entry therefore starts at the top with no visible
         * bottom->top jump.
         */
        show_page(PAGE_MAIN);
        settings_reset_to_top();
    }
}

/* ---------------- Swipe-back helpers ---------------- */

static bool page_supports_swipe_back(void)
{
    /*
     * Settings intentionally excluded: its content is vertically scrollable.
     * Main has nowhere to go back to.
     */
    return current_page == PAGE_XLR ||
           current_page == PAGE_RJ45;
}

static lv_obj_t *active_drag_obj(void)
{
    switch (current_page) {
        case PAGE_XLR:
            return ui_XLR_test;

        case PAGE_RJ45:
            return ui_RJ45_test;

        default:
            return NULL;
    }
}

static void go_back_one_level(void)
{
    switch (current_page) {
        case PAGE_XLR:
        case PAGE_RJ45:
        case PAGE_SETTINGS:
            show_page(PAGE_MAIN);
            break;

        case PAGE_MAIN:
        default:
            break;
    }
}

static void set_translate_y(lv_obj_t *obj, int y)
{
    if (obj) {
        lv_obj_set_style_translate_y(obj, y,
                                     LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void anim_exec_cb(void *var, int32_t value)
{
    lv_obj_t *obj = (lv_obj_t *)var;

    set_translate_y(obj, (int)value);
    drag_translate_y = (int)value;
}

static void anim_ready_cb(lv_anim_t *a)
{
    (void)a;

    if (animation_obj) {
        reset_translate(animation_obj);
    }

    drag_obj = NULL;
    drag_translate_y = 0;

    if (animation_back) {
        go_back_one_level();
        ESP_LOGI(TAG, "slide -> BACK");
    }

    animation_running = false;
    animation_back = false;
    animation_obj = NULL;
}

static void start_animation(lv_obj_t *obj,
                            int from_y,
                            int to_y,
                            bool do_back)
{
    if (!obj) {
        if (do_back) {
            go_back_one_level();
        }
        return;
    }

    animation_running = true;
    animation_back = do_back;
    animation_obj = obj;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_exec_cb);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_time(&a, SWIPE_ANIM_TIME_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, anim_ready_cb);
    lv_anim_start(&a);
}

static void update_drag_visual(void)
{
    if (animation_running ||
        !page_supports_swipe_back() ||
        !touch_pressed ||
        !touch_moved) {
        return;
    }

    int dx = touch_last.x - touch_start.x;
    int dy = touch_last.y - touch_start.y;

    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    /*
     * Only move the page if the gesture is primarily upward.
     * Sideways/downward movement does not translate the page.
     */
    if (dy < 0 && ady > adx) {
        if (!drag_obj) {
            drag_obj = active_drag_obj();
        }

        if (drag_obj) {
            if (dy < -LCD_HEIGHT) {
                dy = -LCD_HEIGHT;
            }

            drag_translate_y = dy;
            set_translate_y(drag_obj, drag_translate_y);
        }
    }
}

static void finish_drag(void)
{
    if (!release_pending) {
        return;
    }

    release_pending = false;

    if (!page_supports_swipe_back() || !touch_moved) {
        if (drag_obj && drag_translate_y != 0) {
            start_animation(drag_obj,
                            drag_translate_y,
                            0,
                            false);
        } else {
            drag_obj = NULL;
            drag_translate_y = 0;
        }

        return;
    }

    int dx = release_dx;
    int dy = release_dy;

    int adx = dx < 0 ? -dx : dx;
    int upward = dy < 0 ? -dy : 0;

    bool vertical_up = dy < 0 && upward > adx;

    /*
     * Dynamic 80% threshold:
     * start y=120 -> require 96 px
     * start y=80  -> require 64 px
     * start y=50  -> require 40 px
     */
    int required = (touch_start.y * SWIPE_BACK_PERCENT) / 100;

    if (required < SWIPE_BACK_MIN_PIXELS) {
        required = SWIPE_BACK_MIN_PIXELS;
    }

    bool pass = vertical_up && upward >= required;

    if (!drag_obj) {
        drag_obj = active_drag_obj();
    }

    if (pass) {
        ESP_LOGI(TAG,
                 "slide BACK startY=%d dx=%d dy=%d required=%d",
                 touch_start.y, dx, dy, required);

        start_animation(drag_obj,
                        drag_translate_y,
                        -LCD_HEIGHT,
                        true);
    } else {
        ESP_LOGI(TAG,
                 "slide CANCEL startY=%d dx=%d dy=%d required=%d",
                 touch_start.y, dx, dy, required);

        if (drag_obj && drag_translate_y != 0) {
            start_animation(drag_obj,
                            drag_translate_y,
                            0,
                            false);
        } else {
            drag_obj = NULL;
            drag_translate_y = 0;
        }
    }
}

/* ---------------- Public API ---------------- */

void navigation_init(void)
{
    /*
     * SquareLine contains no navigation events. Register them here so UI
     * regeneration does not overwrite application behaviour.
     */
    lv_obj_add_event_cb(ui_XLR_menu_button,
                        xlr_button_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_RJ45_menu_button,
                        rj45_button_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_Settings_menu_button,
                        settings_button_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_Back_button,
                        settings_back_button_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    /*
     * Custom Settings scrollbar:
     * - scrolling content moves ScrollThumb
     * - dragging ScrollThumb scrolls content
     */
    lv_obj_set_scroll_dir(ui_Settings_menu_scrollable, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_Settings_menu_scrollable,
                              LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_event_cb(ui_Settings_menu_scrollable,
                        settings_scroll_cb,
                        LV_EVENT_SCROLL,
                        NULL);

    /*
     * Keep receiving PRESSING even when the finger leaves the narrow thumb.
     */
    lv_obj_add_flag(ui_ScrollThumb, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_flag(ui_ScrollThumb, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(ui_ScrollThumb,
                        settings_thumb_pressed_cb,
                        LV_EVENT_PRESSED,
                        NULL);

    lv_obj_add_event_cb(ui_ScrollThumb,
                        settings_thumb_pressing_cb,
                        LV_EVENT_PRESSING,
                        NULL);

    lv_obj_add_event_cb(ui_ScrollThumb,
                        settings_thumb_released_cb,
                        LV_EVENT_RELEASED,
                        NULL);

    lv_obj_add_event_cb(ui_ScrollThumb,
                        settings_thumb_released_cb,
                        LV_EVENT_PRESS_LOST,
                        NULL);

    settings_set_thumb_y(0);
    show_page(PAGE_MAIN);

    ESP_LOGI(TAG, "navigation_init: Settings scroll thumb connected");
}

void navigation_touch_press(int16_t x, int16_t y)
{
    if (animation_running) {
        return;
    }

    touch_pressed = true;
    touch_moved = false;

    touch_start.x = x;
    touch_start.y = y;

    touch_last = touch_start;

    release_pending = false;
    drag_obj = NULL;
    drag_translate_y = 0;
}

void navigation_touch_move(int16_t x, int16_t y)
{
    if (!touch_pressed || animation_running) {
        return;
    }

    touch_last.x = x;
    touch_last.y = y;

    int dx = touch_last.x - touch_start.x;
    int dy = touch_last.y - touch_start.y;

    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;

    if (adx >= TOUCH_MOVE_THRESHOLD ||
        ady >= TOUCH_MOVE_THRESHOLD) {
        touch_moved = true;
    }
}

void navigation_touch_release(int16_t x, int16_t y)
{
    if (!touch_pressed) {
        return;
    }

    touch_last.x = x;
    touch_last.y = y;

    release_dx = touch_last.x - touch_start.x;
    release_dy = touch_last.y - touch_start.y;
    release_pending = true;

    touch_pressed = false;
}

void navigation_process(void)
{
    /*
     * Continuous sync is deliberate. It makes the custom thumb follow
     * normal finger scrolling, momentum, programmatic scroll and layout
     * changes even if a particular LV_EVENT_SCROLL is missed.
     */
    if (current_page == PAGE_SETTINGS && !settings_thumb_dragging) {
        settings_update_thumb_from_scroll();
    }

    update_drag_visual();
    finish_drag();
}