#pragma once
// =============================================================================
// Phoenix Arcade Audio — Simple LEDC, single core, no tasks, no ISR
// =============================================================================
// Keeps it simple: when Z80 writes sound registers, update LEDC frequency.
// One LEDC channel, one frequency at a time (dominant channel wins).
// GPIO18, no ring buffer, no extra tasks, no crashes.
// =============================================================================

#include <Arduino.h>
#include <driver/ledc.h>

#define AUDIO_PIN          18
#define AUDIO_LEDC_TIMER   LEDC_TIMER_1
#define AUDIO_LEDC_CH      LEDC_CHANNEL_0

static uint8_t s_sound_a = 0;
static uint8_t s_sound_b = 0;

// LFSR for noise
static uint32_t s_lfsr = 1;
static uint32_t s_noise_tick = 0;

static const uint32_t E1_HZ[2] = {500, 1000};
static const uint32_t E2_HZ[4] = {1200, 800, 600, 400};
static const uint32_t MEL_HZ[4]= {0, 440, 523, 659};

static void ledc_set_tone(uint32_t freq_hz, uint8_t duty) {
    if (freq_hz < 20 || freq_hz > 20000) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH);
        return;
    }
    ledc_set_freq(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_TIMER, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH);
}

static void phoenix_audio_update() {
    // Called once per frame from LoopZ80 — simple priority: E1 > E2 > melody > noise
    uint8_t sa = s_sound_a, sb = s_sound_b;

    uint8_t e1 = sb & 0x0F;
    if (e1) {
        ledc_set_tone(E1_HZ[(sb>>4)&1] / e1, 128);
        return;
    }
    uint8_t e2 = sa & 0x0F;
    if (e2) {
        ledc_set_tone(E2_HZ[(sa>>4)&3] / e2, 128);
        return;
    }
    uint8_t tune = sb >> 6;
    if (tune && MEL_HZ[tune]) {
        ledc_set_tone(MEL_HZ[tune], 128);
        return;
    }
    if (sa & 0x40) {
        // Noise: toggle LEDC frequency randomly using LFSR
        uint32_t fb = ~((s_lfsr>>17)^(s_lfsr>>16)) & 1;
        s_lfsr = ((s_lfsr<<1)|fb) & 0x3FFFF;
        uint32_t noise_freq = 200 + (s_lfsr & 0x7FF); // 200..2247 Hz
        ledc_set_tone(noise_freq, 128);
        return;
    }
    // Silence
    ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH);
}

static void phoenix_boot_sound() {
    for (int freq = 200; freq <= 2000; freq += 25) {
        ledc_set_tone(freq, 128);
        delay(4);
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH);
    delay(80);
    const int beeps[] = {880, 1100, 1320};
    for (int i = 0; i < 3; i++) {
        ledc_set_tone(beeps[i], 128);
        delay(80);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH);
        delay(40);
    }
}

static void phoenix_audio_init() {
    ledc_timer_config_t tc = {};
    tc.speed_mode      = LEDC_LOW_SPEED_MODE;
    tc.duty_resolution = LEDC_TIMER_8_BIT;
    tc.timer_num       = AUDIO_LEDC_TIMER;
    tc.freq_hz         = 440;
    tc.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&tc);

    ledc_channel_config_t cc = {};
    cc.gpio_num   = AUDIO_PIN;
    cc.speed_mode = LEDC_LOW_SPEED_MODE;
    cc.channel    = AUDIO_LEDC_CH;
    cc.timer_sel  = AUDIO_LEDC_TIMER;
    cc.duty       = 0;
    cc.hpoint     = 0;
    ledc_channel_config(&cc);
}

static inline void phoenix_audio_control_a(uint8_t d) { s_sound_a = d; }
static inline void phoenix_audio_control_b(uint8_t d) { s_sound_b = d; }
