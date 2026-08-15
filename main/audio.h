#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAX98357A wiring used by this project:
 * SD   -> GPIO15
 * BCLK -> GPIO16
 * LRC  -> GPIO17
 * DIN  -> GPIO18
 */
void audio_init(void);

/* Non-blocking UI sounds. */
void audio_play_click(void);
void audio_play_startup(void);

/* Digital PCM volume, 0..100. Reserved for future Settings volume slider. */
void audio_set_volume(uint8_t percent);
uint8_t audio_get_volume(void);

/* Hardware amplifier shutdown/mute using MAX98357A SD pin. */
void audio_set_enabled(int enabled);

#ifdef __cplusplus
}
#endif
