#!/usr/bin/env python3
"""
make_embedded_roms.py  --  Phoenix arcade ROM to C asset converter
Generates phoenix_embedded_assets.c from original ROM files.

GFX ROM layout (MAME charlayout):
  256 tiles, 2bpp, 8x8 pixels per tile.
  Each 0x1000-byte region is split:
    bytes [0x0000..0x07FF] = plane-0 for all 256 tiles  (8 bytes each)
    bytes [0x0800..0x0FFF] = plane-1 for all 256 tiles  (8 bytes each)
  Pixel bit order: bit7 = leftmost pixel, bit0 = rightmost.
  2bpp value = (plane1_bit << 1) | plane0_bit.

  BG region: ic23.3d @ 0x0000, ic24.4d @ 0x0800  -> phoenix_bg_tiles[0x1000]
  FG region: b1-ic39.3b @ 0x0000, b2-ic40.4b @ 0x0800 -> phoenix_fg_tiles[0x1000]

Palette (MAME phoenix_palette):
  Two 256x4-bit PROMs: mmi6301.ic40 (lo), mmi6301.ic41 (hi).
  For each pen index i (0..127):
    prom_idx = bitswap7(i, 6,5,1,0,4,3,2)   <- MAME bit reordering
    lo = ic40[prom_idx] & 0x0F
    hi = ic41[prom_idx] & 0x0F
    R = 0x55*(lo>>0 & 1) + 0xAA*(hi>>0 & 1)
    G = 0x55*(lo>>2 & 1) + 0xAA*(hi>>2 & 1)
    B = 0x55*(lo>>1 & 1) + 0xAA*(hi>>1 & 1)
  Output: phoenix_palette565[128]
  Index from renderer:
    col  = (palette_bank << 4) | layer_base | color_attr
           layer_base = 8 for FG, 0 for BG
           color_attr = tile_code >> 5  (0..7)
           palette_bank = video_reg bit1 (0 or 1)
    pen  = col * 4 + pixel_value (0..3)
    -> palette565[pen]  (pen 0..127)
"""

from __future__ import annotations
from pathlib import Path
import sys

EXPECTED = {
    "ic45":         0x0800,
    "ic46":         0x0800,
    "ic47":         0x0800,
    "ic48":         0x0800,
    "h5-ic49.5a":  0x0800,
    "h6-ic50.6a":  0x0800,
    "h7-ic51.7a":  0x0800,
    "h8-ic52.8a":  0x0800,
    "ic23.3d":      0x0800,
    "ic24.4d":      0x0800,
    "b1-ic39.3b":  0x0800,
    "b2-ic40.4b":  0x0800,
    "mmi6301.ic40": 0x0100,
    "mmi6301.ic41": 0x0100,
}

MAIN_ROM_ORDER = [
    "ic45", "ic46", "ic47", "ic48",
    "h5-ic49.5a", "h6-ic50.6a", "h7-ic51.7a", "h8-ic52.8a",
]


def bitswap7(i: int) -> int:
    """
    MAME bitswap<7>(i, 6, 5, 1, 0, 4, 3, 2):
    Output bit k gets input bit src_bits[k].
    src_bits = [2, 3, 4, 0, 1, 5, 6]  (index = output bit position)
    """
    src_bits = [2, 3, 4, 0, 1, 5, 6]
    return sum(((i >> src_bits[k]) & 1) << k for k in range(7))


def rgb565_palette(prom_lo: bytes, prom_hi: bytes) -> list[int]:
    """
    Generate 128-entry RGB565 palette matching MAME's phoenix_palette().
    For each pen 0..127:
      prom_idx = bitswap7(pen)
      R = 0x55*(lo>>0&1) + 0xAA*(hi>>0&1)
      G = 0x55*(lo>>2&1) + 0xAA*(hi>>2&1)
      B = 0x55*(lo>>1&1) + 0xAA*(hi>>1&1)
    """
    out: list[int] = []
    for pen in range(128):
        idx = bitswap7(pen)
        lo = prom_lo[idx] & 0x0F
        hi = prom_hi[idx] & 0x0F
        r = min(0x55 * ((lo >> 0) & 1) + 0xAA * ((hi >> 0) & 1), 255)
        g = min(0x55 * ((lo >> 2) & 1) + 0xAA * ((hi >> 2) & 1), 255)
        b = min(0x55 * ((lo >> 1) & 1) + 0xAA * ((hi >> 1) & 1), 255)
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(rgb565)
    return out


def emit_c_array_u8(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[0x{len(data):04X}] = {{"]
    for off in range(0, len(data), 12):
        chunk = data[off:off + 12]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def emit_c_array_u16(name: str, data: list[int]) -> str:
    lines = [f"const uint16_t {name}[{len(data)}] = {{"]
    for off in range(0, len(data), 8):
        chunk = data[off:off + 8]
        lines.append("    " + ", ".join(f"0x{v:04X}" for v in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_embedded_roms.py <rom_dir> <output_c>", file=sys.stderr)
        return 2

    rom_dir = Path(sys.argv[1])
    out_c   = Path(sys.argv[2])

    blobs: dict[str, bytes] = {}
    for name, size in EXPECTED.items():
        p = rom_dir / name
        data = p.read_bytes()
        if len(data) != size:
            raise SystemExit(f"size mismatch for {name}: got {len(data)}, expected {size}")
        blobs[name] = data

    # CPU ROM: 8 x 0x0800 = 0x4000 bytes
    prog = b"".join(blobs[n] for n in MAIN_ROM_ORDER)

    # GFX ROMs: each pair concatenated -> 0x1000 bytes
    #   ic23.3d  [0x0000..0x07FF] = BG plane-0 (all 256 tiles, 8 bytes each)
    #   ic24.4d  [0x0800..0x0FFF] = BG plane-1 (all 256 tiles, 8 bytes each)
    bg = blobs["ic23.3d"] + blobs["ic24.4d"]

    #   b1-ic39.3b [0x0000..0x07FF] = FG plane-0
    #   b2-ic40.4b [0x0800..0x0FFF] = FG plane-1
    fg = blobs["b1-ic39.3b"] + blobs["b2-ic40.4b"]

    # Palette: 128 entries with MAME-accurate bitswap7 PROM indexing
    pal = rgb565_palette(blobs["mmi6301.ic40"], blobs["mmi6301.ic41"])

    text = [
        '#include "phoenix_embedded_assets.h"',
        "",
        "/* CPU program ROM: ic45-ic48 + h5-h8 = 0x4000 bytes */",
        emit_c_array_u8("phoenix_prog_rom", prog),
        "",
        "/* BG tile GFX: ic23.3d (plane0) + ic24.4d (plane1) = 0x1000 bytes */",
        "/* Layout: bytes[0x000..0x7FF]=plane0 all 256 tiles, bytes[0x800..0xFFF]=plane1 */",
        emit_c_array_u8("phoenix_bg_tiles", bg),
        "",
        "/* FG tile GFX: b1-ic39.3b (plane0) + b2-ic40.4b (plane1) = 0x1000 bytes */",
        emit_c_array_u8("phoenix_fg_tiles", fg),
        "",
        "/* Palette: 128 RGB565 entries, pen = col*4+pixel */",
        "/* col = (palette_bank<<4) | layer_base | color_attr */",
        "/* layer_base: FG=8, BG=0.  color_attr = tile_code>>5 (0-7) */",
        "/* Derived from mmi6301.ic40/ic41 with MAME bitswap7(pen,6,5,1,0,4,3,2) */",
        emit_c_array_u16("phoenix_palette565", pal),
        "",
    ]
    out_c.write_text("\n".join(text), encoding="utf-8")
    print(f"Written {out_c}  ({len(prog)} prog + {len(bg)} bg + {len(fg)} fg + {len(pal)} palette entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
