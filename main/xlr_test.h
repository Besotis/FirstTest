#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XLR Normal Mode pin assignment
 *
 * Side A:
 *   Pin 1 -> GPIO40
 *   Pin 2 -> GPIO41
 *   Pin 3 -> GPIO42
 *
 * Side B:
 *   Pin 1 -> GPIO47
 *   Pin 2 -> GPIO48
 *   Pin 3 -> GPIO21
 *
 * Normal test:
 * - 256 complete 6x6 matrix scans per second
 * - 128 scans are accumulated into one 0.5 s result cycle
 * - one bad scan is enough to mark that 0.5 s cycle bad
 * - 10 consecutive perfect cycles fill Cable Quality from 0% to 100%
 * - test keeps running at 100% until XLR page is closed/back
 */
void xlr_test_init(void);
void xlr_test_start(void);
void xlr_test_stop(void);
int  xlr_test_is_running(void);

/*
 * Call from the LVGL/main loop.
 * Scanner task never touches LVGL directly; this function safely applies
 * pending 0.5 s results to the SquareLine widgets.
 */
void xlr_test_process(void);

#ifdef __cplusplus
}
#endif
