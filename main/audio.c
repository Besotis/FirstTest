#include "audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/*
 * MAX98357A wiring
 *
 * VIN  -> 3.3V
 * GND  -> GND
 * SD   -> GPIO15
 * BCLK -> GPIO16
 * LRC  -> GPIO17
 * DIN  -> GPIO18
 * GAIN -> not connected
 */

#define AUDIO_PIN_SD        15
#define AUDIO_PIN_BCLK      16
#define AUDIO_PIN_WS        17
#define AUDIO_PIN_DOUT      18

#define AUDIO_SAMPLE_RATE   44100

#define AUDIO_QUEUE_LEN     8
#define AUDIO_TASK_STACK    4096
#define AUDIO_TASK_PRIO     4

/* Overall digital volume. Future Settings slider can call audio_set_volume(). */
static uint8_t volume_percent = 35;

static const char *TAG = "AUDIO";

static i2s_chan_handle_t tx_chan = NULL;
static QueueHandle_t audio_queue = NULL;
static bool amp_enabled = false;

/* 32-point signed 16-bit sine lookup table */
static const int16_t sine32[32] = {
      0,  6393, 12539, 18204, 23170, 27245, 30273, 32137,
  32767, 32137, 30273, 27245, 23170, 18204, 12539,  6393,
      0, -6393,-12539,-18204,-23170,-27245,-30273,-32137,
 -32767,-32137,-30273,-27245,-23170,-18204,-12539, -6393
};

typedef enum {
    AUDIO_CMD_CLICK = 0,
    AUDIO_CMD_STARTUP,
} audio_cmd_t;

static int16_t apply_master_volume(int32_t sample)
{
    sample = (sample * volume_percent) / 100;

    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;

    return (int16_t)sample;
}

static void write_stereo_samples(const int16_t *mono, size_t sample_count)
{
    int16_t stereo[128 * 2];

    while (sample_count > 0) {
        size_t n = sample_count > 128 ? 128 : sample_count;

        for (size_t i = 0; i < n; i++) {
            int16_t s = apply_master_volume(mono[i]);

            /* Same sample in both slots. */
            stereo[i * 2]     = s;
            stereo[i * 2 + 1] = s;
        }

        size_t bytes_written = 0;

        esp_err_t err = i2s_channel_write(
            tx_chan,
            stereo,
            n * 2 * sizeof(int16_t),
            &bytes_written,
            portMAX_DELAY
        );

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
            return;
        }

        mono += n;
        sample_count -= n;
    }
}

static void play_silence_ms(uint32_t duration_ms)
{
    static const int16_t silence[128] = {0};

    uint32_t remaining =
        (AUDIO_SAMPLE_RATE * duration_ms) / 1000U;

    while (remaining > 0) {
        uint32_t n = remaining > 128 ? 128 : remaining;
        write_stereo_samples(silence, n);
        remaining -= n;
    }
}

/*
 * Smooth tone with a short attack/release envelope.
 * local_level_percent is applied before the global volume.
 *
 * This removes the hard edges that made the old beeps sound harsh/clicky.
 */
static void play_tone_enveloped(uint32_t frequency_hz,
                                uint32_t duration_ms,
                                uint8_t local_level_percent,
                                uint32_t attack_ms,
                                uint32_t release_ms)
{
    if (!amp_enabled || frequency_hz == 0 || duration_ms == 0) {
        return;
    }

    if (local_level_percent > 100) {
        local_level_percent = 100;
    }

    uint32_t total_samples =
        (AUDIO_SAMPLE_RATE * duration_ms) / 1000U;

    uint32_t attack_samples =
        (AUDIO_SAMPLE_RATE * attack_ms) / 1000U;

    uint32_t release_samples =
        (AUDIO_SAMPLE_RATE * release_ms) / 1000U;

    if (attack_samples + release_samples > total_samples) {
        attack_samples = total_samples / 3;
        release_samples = total_samples / 3;
    }

    uint32_t phase = 0;
    uint32_t phase_step =
        (uint32_t)(((uint64_t)frequency_hz << 32) / AUDIO_SAMPLE_RATE);

    uint32_t generated = 0;
    int16_t mono[128];

    while (generated < total_samples) {
        uint32_t n = total_samples - generated;
        if (n > 128) n = 128;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t pos = generated + i;
            uint32_t index = phase >> 27;
            int32_t sample = sine32[index & 31U];
            phase += phase_step;

            /* Envelope represented as 0..1000 */
            uint32_t env = 1000;

            if (attack_samples > 0 && pos < attack_samples) {
                env = (pos * 1000U) / attack_samples;
            }

            uint32_t samples_left = total_samples - pos;
            if (release_samples > 0 && samples_left <= release_samples) {
                uint32_t rel =
                    (samples_left * 1000U) / release_samples;
                if (rel < env) env = rel;
            }

            sample = (sample * (int32_t)local_level_percent) / 100;
            sample = (sample * (int32_t)env) / 1000;

            mono[i] = (int16_t)sample;
        }

        write_stereo_samples(mono, n);
        generated += n;
    }
}

/*
 * A more tactile UI "tick":
 * two very short overlapping-feeling tonal transients, rather than
 * the old long high-pitched beep.
 */

/*
 * Percussive "thump" generator.
 *
 * Unlike play_tone_enveloped(), this does not hold one musical pitch.
 * The frequency falls quickly from start_hz to end_hz while the amplitude
 * decays. This gives a short "BAM / THUD" impression rather than a beep.
 */
static void play_thump(uint32_t start_hz,
                       uint32_t end_hz,
                       uint32_t duration_ms,
                       uint8_t local_level_percent)
{
    if (!amp_enabled || duration_ms == 0) {
        return;
    }

    if (local_level_percent > 100) {
        local_level_percent = 100;
    }

    uint32_t total_samples =
        (AUDIO_SAMPLE_RATE * duration_ms) / 1000U;

    if (total_samples == 0) {
        return;
    }

    uint32_t phase = 0;
    uint32_t generated = 0;
    int16_t mono[128];

    while (generated < total_samples) {
        uint32_t n = total_samples - generated;
        if (n > 128) n = 128;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t pos = generated + i;

            /* Progress 0..1000 through the hit. */
            uint32_t p = (pos * 1000U) / total_samples;

            /*
             * Fast downward pitch sweep. Most of the pitch drop happens
             * near the beginning, which sounds more like an impact.
             */
            uint32_t curved = (p * p) / 1000U;
            uint32_t freq =
                start_hz -
                ((start_hz - end_hz) * curved) / 1000U;

            uint32_t phase_step =
                (uint32_t)(((uint64_t)freq << 32) / AUDIO_SAMPLE_RATE);

            uint32_t index = phase >> 27;
            int32_t sample = sine32[index & 31U];
            phase += phase_step;

            /*
             * Percussive envelope:
             * very fast attack, then strong nonlinear decay.
             */
            uint32_t env;

            const uint32_t attack_samples =
                (AUDIO_SAMPLE_RATE * 2U) / 1000U;

            if (pos < attack_samples && attack_samples > 0) {
                env = (pos * 1000U) / attack_samples;
            } else {
                uint32_t remain =
                    ((total_samples - pos) * 1000U) / total_samples;

                /* Squared decay makes the tail disappear quickly. */
                env = (remain * remain) / 1000U;
            }

            sample =
                (sample * (int32_t)local_level_percent) / 100;
            sample =
                (sample * (int32_t)env) / 1000;

            mono[i] = (int16_t)sample;
        }

        write_stereo_samples(mono, n);
        generated += n;
    }
}


/*
 * Low-frequency impact pulse for a dull "BUM/BAM".
 *
 * This is deliberately NOT a normal repeating tone. It produces only a
 * short low-frequency wave packet with a fast attack and heavy decay.
 * That greatly reduces the audible "beep" character.
 */
static void play_impact(uint32_t frequency_hz,
                        uint32_t duration_ms,
                        uint8_t local_level_percent)
{
    if (!amp_enabled || frequency_hz == 0 || duration_ms == 0) {
        return;
    }

    if (local_level_percent > 100) {
        local_level_percent = 100;
    }

    uint32_t total_samples =
        (AUDIO_SAMPLE_RATE * duration_ms) / 1000U;

    if (total_samples == 0) {
        return;
    }

    uint32_t phase = 0;
    uint32_t phase_step =
        (uint32_t)(((uint64_t)frequency_hz << 32) / AUDIO_SAMPLE_RATE);

    uint32_t generated = 0;
    int16_t mono[128];

    while (generated < total_samples) {
        uint32_t n = total_samples - generated;
        if (n > 128) n = 128;

        for (uint32_t i = 0; i < n; i++) {
            uint32_t pos = generated + i;
            uint32_t index = phase >> 27;

            int32_t sample = sine32[index & 31U];
            phase += phase_step;

            /*
             * Very short 2 ms attack, followed by a cubic decay.
             * Cubic decay removes the ringing tail much faster than a
             * conventional tone envelope.
             */
            uint32_t env;
            uint32_t attack_samples =
                (AUDIO_SAMPLE_RATE * 2U) / 1000U;

            if (attack_samples > 0 && pos < attack_samples) {
                env = (pos * 1000U) / attack_samples;
            } else {
                uint32_t remain =
                    ((total_samples - pos) * 1000U) / total_samples;

                env = (remain * remain) / 1000U;
                env = (env * remain) / 1000U;
            }

            sample =
                (sample * (int32_t)local_level_percent) / 100;
            sample =
                (sample * (int32_t)env) / 1000;

            mono[i] = (int16_t)sample;
        }

        write_stereo_samples(mono, n);
        generated += n;
    }
}

static void play_click_sound(void)
{
    audio_set_enabled(1);
    vTaskDelay(pdMS_TO_TICKS(2));

    /*
     * Dull mechanical impact.
     *
     * The first pulse supplies the body of the "BUM".
     * The second, much quieter pulse makes it feel less synthetic.
     *
     * For an even deeper sound try:
     *   play_impact(75, 65, 100);
     *
     * Note: the physical speaker ultimately determines how much real bass
     * can be reproduced.
     */
    play_impact(60, 58, 100);

    play_silence_ms(4);

    play_impact(40, 32, 48);

    play_silence_ms(25);
    audio_set_enabled(0);
}

/*
 * Soft two-note startup chime:
 * low short chirp -> tiny pause -> slightly higher chirp.
 */
static void play_startup_sound(void)
{
    audio_set_enabled(1);
    vTaskDelay(pdMS_TO_TICKS(2));

    play_tone_enveloped(6000, 48, 55, 6, 12);
    play_silence_ms(34);
    play_tone_enveloped(4000, 62, 58, 6, 16);

    play_silence_ms(24);
    audio_set_enabled(0);
}

static void audio_task(void *arg)
{
    (void)arg;

    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(audio_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd) {
                case AUDIO_CMD_CLICK:
                    play_click_sound();
                    break;

                case AUDIO_CMD_STARTUP:
                    play_startup_sound();
                    break;

                default:
                    break;
            }
        }
    }
}

static void queue_sound(audio_cmd_t cmd)
{
    if (!audio_queue) {
        return;
    }

    /*
     * UI must never wait for audio.
     * If the queue is full, drop the newest click instead of blocking LVGL.
     */
    (void)xQueueSend(audio_queue, &cmd, 0);
}

void audio_set_enabled(int enabled)
{
    amp_enabled = enabled != 0;
    gpio_set_level(AUDIO_PIN_SD, amp_enabled ? 1 : 0);
}

void audio_set_volume(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    volume_percent = percent;
}

uint8_t audio_get_volume(void)
{
    return volume_percent;
}

void audio_play_click(void)
{
    queue_sound(AUDIO_CMD_CLICK);
}

void audio_play_startup(void)
{
    queue_sound(AUDIO_CMD_STARTUP);
}

void audio_init(void)
{
    ESP_LOGI(TAG, "Initializing MAX98357A");

    gpio_config_t sd_cfg = {
        .pin_bit_mask = 1ULL << AUDIO_PIN_SD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&sd_cfg));

    /* Keep amplifier muted while I2S is configured. */
    audio_set_enabled(0);

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    ESP_ERROR_CHECK(
        i2s_new_channel(&chan_cfg, &tx_chan, NULL)
    );

    i2s_std_slot_config_t slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
        );

    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    i2s_std_config_t std_cfg = {
        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),

        .slot_cfg = slot_cfg,

        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = AUDIO_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,

            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(
        i2s_channel_init_std_mode(tx_chan, &std_cfg)
    );

    ESP_ERROR_CHECK(
        i2s_channel_enable(tx_chan)
    );

    audio_queue =
        xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_cmd_t));

    if (!audio_queue) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return;
    }

    BaseType_t task_ok =
        xTaskCreate(
            audio_task,
            "audio",
            AUDIO_TASK_STACK,
            NULL,
            AUDIO_TASK_PRIO,
            NULL
        );

    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        return;
    }

    /*
     * Idle state is muted. Individual sounds briefly enable MAX98357A,
     * write their samples + silence tail, and mute it again.
     */
    audio_set_enabled(0);

    ESP_LOGI(TAG,
             "MAX98357A ready: SD=%d BCLK=%d WS=%d DIN=%d volume=%u%%",
             AUDIO_PIN_SD,
             AUDIO_PIN_BCLK,
             AUDIO_PIN_WS,
             AUDIO_PIN_DOUT,
             (unsigned)volume_percent);
}