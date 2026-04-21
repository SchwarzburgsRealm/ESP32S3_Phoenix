#pragma once

#include <stdint.h>

#define SCREEN_W 320
#define SCREEN_H 240

typedef struct {
    uint16_t fb[SCREEN_W * SCREEN_H];
    volatile bool frame_ready;
} lcd_t;

extern lcd_t lcd;
