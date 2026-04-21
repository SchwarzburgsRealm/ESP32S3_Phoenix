#pragma once

#include <stdint.h>

// LCD pin definitions - adjust these for your wiring
#ifndef TFT_PIN_CS
#define TFT_PIN_CS   7
#endif
#ifndef TFT_PIN_DC
#define TFT_PIN_DC   8
#endif
#ifndef TFT_PIN_RST
#define TFT_PIN_RST  9
#endif
#ifndef TFT_PIN_MOSI
#define TFT_PIN_MOSI 5
#endif
#ifndef TFT_PIN_SCLK
#define TFT_PIN_SCLK 6
#endif
#ifndef TFT_PIN_MISO
#define TFT_PIN_MISO 13
#endif

// LCD dimensions for 320x240 display
#define SCREEN_W     320
#define SCREEN_H     240

void tft_init(void);
void tft_send_frame(const uint16_t *fb);
void tft_wait_frame(void);

extern volatile uint32_t tft_frame_tx_count;
