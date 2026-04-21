#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU program ROM: 0x4000 bytes (8 x 2KB chips concatenated) */
extern const uint8_t  phoenix_prog_rom[0x4000];

/* BG tile GFX ROM: 0x1000 bytes
   [0x0000..0x07FF] = plane-0 for all 256 tiles (8 bytes per tile)
   [0x0800..0x0FFF] = plane-1 for all 256 tiles (8 bytes per tile)
   2bpp: pixel = (plane1_bit << 1) | plane0_bit, bit7 = leftmost pixel */
extern const uint8_t  phoenix_bg_tiles[0x1000];

/* FG tile GFX ROM: 0x1000 bytes, same layout as BG */
extern const uint8_t  phoenix_fg_tiles[0x1000];

/* Palette: 128 RGB565 entries.
   Index = col * 4 + pixel  (pixel = 2bpp value 0-3)
   col   = (palette_bank << 4) | layer_base | color_attr
   layer_base: FG = 8, BG = 0
   color_attr = tile_code >> 5  (0-7)
   palette_bank = video_reg bit1 (0 or 1)
   Generated from mmi6301.ic40/ic41 with MAME bitswap7(pen,6,5,1,0,4,3,2) */
extern const uint16_t phoenix_palette565[128];

#ifdef __cplusplus
}
#endif
