#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import sys

EXPECTED = {
    "ic45": 0x0800,
    "ic46": 0x0800,
    "ic47": 0x0800,
    "ic48": 0x0800,
    "h5-ic49.5a": 0x0800,
    "h6-ic50.6a": 0x0800,
    "h7-ic51.7a": 0x0800,
    "h8-ic52.8a": 0x0800,
    "ic23.3d": 0x0800,
    "ic24.4d": 0x0800,
    "b1-ic39.3b": 0x0800,
    "b2-ic40.4b": 0x0800,
    "mmi6301.ic40": 0x0100,
    "mmi6301.ic41": 0x0100,
}

MAIN_ROM_ORDER = [
    "ic45", "ic46", "ic47", "ic48",
    "h5-ic49.5a", "h6-ic50.6a", "h7-ic51.7a", "h8-ic52.8a",
]

def rgb565_palette(prom_lo: bytes, prom_hi: bytes) -> list[int]:
    out: list[int] = []
    for i in range(64):
        lo = prom_lo[i] & 0x0F
        hi = prom_hi[i] & 0x0F
        r = 0x55 * ((lo >> 0) & 1) + 0xAA * ((hi >> 0) & 1)
        g = 0x55 * ((lo >> 2) & 1) + 0xAA * ((hi >> 2) & 1)
        b = 0x55 * ((lo >> 1) & 1) + 0xAA * ((hi >> 1) & 1)
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(rgb565)
    return out


def emit_c_array_u8(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[0x{len(data):04X}] = {{"]
    for off in range(0, len(data), 12):
        chunk = data[off:off+12]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def emit_c_array_u16(name: str, data: list[int]) -> str:
    lines = [f"const uint16_t {name}[{len(data)}] = {{"]
    for off in range(0, len(data), 8):
        chunk = data[off:off+8]
        lines.append("    " + ", ".join(f"0x{v:04X}" for v in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_embedded_roms.py <rom_dir> <output_c>", file=sys.stderr)
        return 2

    rom_dir = Path(sys.argv[1])
    out_c = Path(sys.argv[2])

    blobs: dict[str, bytes] = {}
    for name, size in EXPECTED.items():
        p = rom_dir / name
        data = p.read_bytes()
        if len(data) != size:
            raise SystemExit(f"size mismatch for {name}: got {len(data)}, expected {size}")
        blobs[name] = data

    prog = b"".join(blobs[n] for n in MAIN_ROM_ORDER)
    bg = blobs["ic23.3d"] + blobs["ic24.4d"]
    fg = blobs["b1-ic39.3b"] + blobs["b2-ic40.4b"]
    pal = rgb565_palette(blobs["mmi6301.ic40"], blobs["mmi6301.ic41"])

    text = [
        '#include "phoenix_embedded_assets.h"',
        "",
        emit_c_array_u8("phoenix_prog_rom", prog),
        "",
        emit_c_array_u8("phoenix_bg_tiles", bg),
        "",
        emit_c_array_u8("phoenix_fg_tiles", fg),
        "",
        emit_c_array_u16("phoenix_palette565", pal),
        "",
    ]
    out_c.write_text("\n".join(text), encoding="utf-8")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
