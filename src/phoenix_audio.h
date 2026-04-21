#pragma once
// =============================================================================
// Phoenix Arcade Audio — Ring Buffer + LEDC PWM
// =============================================================================
// Architecture:
//   - Core 0: audio_task fills IRAM ring buffer with synthesized samples
//   - esp_timer ISR at 8kHz: pulls samples from buffer → LEDC duty
//   - Fixed 250kHz PWM carrier, duty-cycle modulation for audio
// =============================================================================

#include <Arduino.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// --- Configuration ---
#define AUDIO_PIN           18
#define AUDIO_PWM_FREQ      250000  // Fixed 250kHz PWM carrier
#define AUDIO_SAMPLE_RATE   8000    // 8kHz output rate
#define AUDIO_BUF_SIZE      256     // Ring buffer size (must be power of 2)
#define AUDIO_BUF_MASK      (AUDIO_BUF_SIZE - 1)

#define AUDIO_LEDC_CH       LEDC_CHANNEL_0
#define AUDIO_LEDC_TIMER    LEDC_TIMER_1

// =============================================================================
// Audio State (all in DRAM - safe for ISR access)
// =============================================================================
static uint8_t  s_audio_buf[AUDIO_BUF_SIZE];
static volatile uint32_t s_buf_write = 0;  // Write index (Core 0)
static volatile uint32_t s_buf_read = 0;   // Read index (ISR)

// Sound Registers (written by Z80 on Core 1, read by synth on Core 0)
static volatile uint8_t s_sound_a = 0;
static volatile uint8_t s_sound_b = 0;

// Synth State (Core 0 only)
static struct {
    uint32_t e1_phase, e1_inc;
    uint32_t e2_phase, e2_inc;
    uint32_t mel_phase, mel_inc;
    uint32_t lfsr;
    uint8_t  e1_en, e2_en, mel_en, noise_en;
} s_syn;

// Frequency tables
static const uint16_t E1_HZ[2] = {500, 1000};
static const uint16_t E2_HZ[4] = {1200, 800, 600, 400};
static const uint16_t MEL_HZ[4] = {0, 440, 523, 659};

static inline uint32_t freq_to_inc(uint32_t freq_hz) {
    return (freq_hz << 16) / AUDIO_SAMPLE_RATE;  // Phase increment for 8kHz
}

// =============================================================================
// Synthesize one sample (square waves + noise)
// =============================================================================
static uint8_t synth_sample() {
    int16_t mix = 0;

    // Effect 1 (Sound B) - Background/Shoot
    if (s_syn.e1_en && s_syn.e1_inc) {
        s_syn.e1_phase += s_syn.e1_inc;
        mix += (s_syn.e1_phase < 0x80000000u) ? 70 : -70;
    }

    // Effect 2 (Sound A) - Ship/Enemy
    if (s_syn.e2_en && s_syn.e2_inc) {
        s_syn.e2_phase += s_syn.e2_inc;
        mix += (s_syn.e2_phase < 0x80000000u) ? 70 : -70;
    }

    // Melody
    if (s_syn.mel_en && s_syn.mel_inc) {
        s_syn.mel_phase += s_syn.mel_inc;
        mix += (s_syn.mel_phase < 0x80000000u) ? 70 : -70;
    }

    // Noise (enabled by Sound A bit 6)
    if (s_syn.noise_en) {
        uint32_t fb = ~((s_syn.lfsr >> 17) ^ (s_syn.lfsr >> 16)) & 1;
        s_syn.lfsr = ((s_syn.lfsr << 1) | fb) & 0x3FFFF;
        mix += (s_syn.lfsr & 1) ? 90 : -90;
    }

    // Clip and convert to duty (128 = silence/50%)
    if (mix > 127) mix = 127;
    if (mix < -128) mix = -128;
    return 128 + (int8_t)mix;
}

// =============================================================================
// Audio Task on Core 0 - Direct 8kHz output
// =============================================================================
static void audio_task(void* arg) {
    (void)arg;
    uint32_t last_us = micros();
    const uint32_t period_us = 1000000 / AUDIO_SAMPLE_RATE;  // 125us for 8kHz

    while (1) {
        uint32_t now = micros();
        if (now - last_us >= period_us) {
            last_us += period_us;

            // Synthesize and output directly
            uint8_t duty = synth_sample();
            ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH, duty);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CH);
        } else {
            // Yield to prevent watchdog
            vTaskDelay(1);
        }
    }
}

// =============================================================================
// Public API
// =============================================================================
static void phoenix_audio_init() {
    // Clear state
    memset(&s_syn, 0, sizeof(s_syn));
    s_syn.lfsr = 1;

    // LEDC timer: fixed 250kHz
    ledc_timer_config_t tc = {};
    tc.speed_mode = LEDC_LOW_SPEED_MODE;
    tc.duty_resolution = LEDC_TIMER_8_BIT;
    tc.timer_num = AUDIO_LEDC_TIMER;
    tc.freq_hz = AUDIO_PWM_FREQ;
    tc.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&tc);

    // LEDC channel
    ledc_channel_config_t cc = {};
    cc.speed_mode = LEDC_LOW_SPEED_MODE;
    cc.channel = AUDIO_LEDC_CH;
    cc.timer_sel = AUDIO_LEDC_TIMER;
    cc.gpio_num = AUDIO_PIN;
    cc.duty = 128;  // 50% = silence
    cc.hpoint = 0;
    ledc_channel_config(&cc);

    // Create audio task on Core 0 (outputs 8kHz directly)
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 5, NULL, 0);

    Serial.printf("[AUDIO] 8kHz @ 250kHz PWM on GPIO%d\n", AUDIO_PIN);
}

// Sound A (0x6000) - Effect 2 + Noise enable
static inline void phoenix_audio_control_a(uint8_t data) {
    s_sound_a = data;

    // Effect 2 (bits 3:0 = divisor, bits 5:4 = cap select)
    uint8_t e2_data = data & 0x0F;
    if (e2_data == 0) {
        s_syn.e2_en = 0;
    } else {
        uint8_t sel = (data >> 4) & 0x03;
        uint32_t freq = E2_HZ[sel] / e2_data;
        if (freq < 50) freq = 50;
        if (freq > 8000) freq = 8000;
        s_syn.e2_inc = freq_to_inc(freq);
        s_syn.e2_en = 1;
    }

    // Noise enable (bit 6)
    s_syn.noise_en = (data & 0x40) ? 1 : 0;
}

// Sound B (0x6800) - Effect 1 + Melody
static inline void phoenix_audio_control_b(uint8_t data) {
    s_sound_b = data;

    // Effect 1 (bits 3:0 = divisor, bit 4 = freq range)
    uint8_t e1_data = data & 0x0F;
    if (e1_data == 0) {
        // Check melody (bits 7:6)
        uint8_t tune = data >> 6;
        if (tune && MEL_HZ[tune]) {
            s_syn.mel_inc = freq_to_inc(MEL_HZ[tune]);
            s_syn.mel_en = 1;
            s_syn.e1_en = 0;
        } else {
            s_syn.e1_en = 0;
            s_syn.mel_en = 0;
        }
    } else {
        uint8_t sel = (data >> 4) & 0x01;
        uint32_t freq = E1_HZ[sel] / e1_data;
        if (freq < 50) freq = 50;
        if (freq > 8000) freq = 8000;
        s_syn.e1_inc = freq_to_inc(freq);
        s_syn.e1_en = 1;
        s_syn.mel_en = 0;
    }
}

static inline void phoenix_audio_update() {}

static inline void phoenix_boot_sound() {
    // Quick test beep
    s_syn.e1_inc = freq_to_inc(1000);
    s_syn.e1_en = 1;
    delay(150);
    s_syn.e1_en = 0;
}
