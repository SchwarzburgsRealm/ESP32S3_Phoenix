#pragma once
// =============================================================================
// Phoenix Arcade — Button Input Handler
// =============================================================================
// AUDIO_PIN = 18 (LEDC PWM) — wird NIE als INPUT gesetzt!
// Hardware: 10k Pullup nach VCC, Button schliesst nach GND -> INPUT
// =============================================================================

#include <Arduino.h>
#include <driver/ledc.h>

#define BTN_RIGHT_PIN   10
#define BTN_LEFT_PIN     3
#define BTN_A_PIN       15   // FIRE
#define BTN_B_PIN       16   // SHIELD
#define BTN_SELECT_PIN  46   // COIN
#define BTN_START_PIN   21   // START 1P



static void phoenix_input_init() {
    // GPIO18 = AUDIO LEDC -> NICHT anfassen!
    pinMode(BTN_RIGHT_PIN,  INPUT);  // 10
    pinMode(BTN_LEFT_PIN,   INPUT);  //  3
    pinMode(BTN_A_PIN,      INPUT);  // 15
    pinMode(BTN_B_PIN,      INPUT);  // 16
    pinMode(BTN_SELECT_PIN, INPUT);  // 46
    pinMode(BTN_START_PIN,  INPUT);  // 21
}

static inline uint8_t phoenix_read_inputs() {
    uint8_t in0 = 0xFF;
    if (!digitalRead(BTN_SELECT_PIN)) in0 &= ~0x01;  // COIN
    if (!digitalRead(BTN_START_PIN))  in0 &= ~0x02;  // START1
    if (!digitalRead(BTN_A_PIN))      in0 &= ~0x10;  // FIRE
    if (!digitalRead(BTN_RIGHT_PIN))  in0 &= ~0x20;  // RIGHT
    if (!digitalRead(BTN_LEFT_PIN))   in0 &= ~0x40;  // LEFT
    if (!digitalRead(BTN_B_PIN))      in0 &= ~0x80;  // SHIELD
    return in0;
}
