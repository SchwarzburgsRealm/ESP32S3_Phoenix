# Phoenix Arcade Emulator — ESP32-S3
(C) 2025 Marc Schwarzburg

A hardware emulator of the 1980 **Phoenix** arcade game running on a **WEMOS LOLIN S3** (ESP32-S3), with ILI9341 TFT display, PWM audio and physical buttons.


---

## Hardware

| Component | Part |
|---|---|
| MCU | WEMOS LOLIN S3 (ESP32-S3, 240MHz, 16MB Flash, 8MB PSRAM) |
| Display | ILI9341 2.8" TFT, 320×240, SPI |
| Audio | PWM via RC low-pass filter → Speaker/Amp |
| Buttons | 7× tactile switch, 10kΩ pullup to VCC, closes to GND |

---

## Wiring

### TFT Display (ILI9341)

| Signal | GPIO |
|---|---|
| MOSI | 5 |
| SCLK | 6 |
| CS | 7 |
| DC | 8 |
| RST | 9 |
| MISO | 13 |
| MADCTL | `0x28` (landscape, BGR) |

### Audio

| Signal | GPIO | Notes |
|---|---|---|
| PWM out | **18** | → 1kΩ → Speaker+ / Amp in |
| GND | GND | Speaker− |

Optional: 10nF cap from GPIO18 to GND for extra HF filtering.

### Buttons (10kΩ pullup to 3.3V, closes to GND)

| Function | GPIO | Phoenix Bit |
|---|---|---|
| RIGHT | 10 | IN0 bit 5 |
| LEFT | 3 | IN0 bit 6 |
| FIRE (A) | 15 | IN0 bit 4 |
| SHIELD (B) | 16 | IN0 bit 7 |
| COIN / INSERT | 46 | IN0 bit 0 |
| START 1P | 21 | IN0 bit 1 |

> ⚠️ **GPIO18 = Audio PWM — do not connect a button here!**
> GPIO 11 = Flash pin (always LOW), GPIO 17 = internally used — avoid both.

---

## Software

### Requirements

- [PlatformIO](https://platformio.org/) with Espressif32 platform 6.x
- Arduino framework
- Library: `bitbank2/JPEGDEC @ ^1.8.2`

### ROM Files (not included)

You need the original Phoenix ROM files. Place them in a `rom/` folder:

```
rom/
  ic45          ic46          ic47          ic48
  h5-ic49.5a   h6-ic50.6a   h7-ic51.7a   h8-ic52.8a
  ic23.3d      ic24.4d
  b1-ic39.3b   b2-ic40.4b
  mmi6301.ic40  mmi6301.ic41
```

Generate the embedded C asset file once:

```bash
python src/make_embedded_roms.py rom/ src/phoenix_embedded_assets.c
```

Then build and flash normally — no ROMs needed at build time.

> Add `rom/` to `.gitignore` to keep original ROM files off the repository.

### Build & Flash

```bash
pio run -t upload
pio device monitor   # 115200 baud
```

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    ESP32-S3                          │
│                                                     │
│  Core 1 (main)                                      │
│  ┌──────────────────────────────────────────────┐   │
│  │ Z80 Emulator (RunZ80, IPeriod=88000)         │   │
│  │   ↓ every frame (~16ms)                      │   │
│  │ LoopZ80():                                   │   │
│  │   phoenix_read_inputs() → input_0            │   │
│  │   phoenix_audio_update() → LEDC freq         │   │
│  │   render_screen() → framebuffer → SPI TFT   │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  Z80 Memory Map:                                    │
│   0x0000-0x3FFF  ROM (16KB)                        │
│   0x4000-0x4FFF  Video RAM (2 banks, 4KB each)     │
│   0x5000         Video register (bank + palette)    │
│   0x5800         BG scroll register                 │
│   0x6000         Sound A                           │
│   0x6800         Sound B                           │
│   0x7000         IN0 (buttons)                     │
│   0x7800         DSW0 (DIP switches + VBlank bit7) │
└─────────────────────────────────────────────────────┘
```

### Video

- Native Phoenix resolution: **256×208 pixels** (32×26 tiles of 8×8)
- Displayed centred on 320×240 LCD with 32px left/right border, 16px top/bottom
- Two tile layers: Background (scrollable) + Foreground (transparent pixel 0)
- 2bpp graphics, 128-entry RGB565 palette derived from original colour PROMs
- VBlank emulated via DSW0 bit 7 toggling at 60Hz using `micros()`

### Audio

- **Sound A** (0x6000): Effect 2 tone + noise enable
- **Sound B** (0x6800): Effect 1 tone + MM6221AA melody select
- PWM carrier: ~312kHz, 8-bit duty → LEDC channel 0 → GPIO18
- Boot jingle plays on startup (sweep + 3 beeps)

### Performance optimisations

- `IRAM_ATTR` on all hot-path functions (RdZ80, WrZ80, LoopZ80, render_*)
- `-O3 -funroll-loops -finline-functions` compiler flags
- `CORE_DEBUG_LEVEL=0` — ESP logging completely disabled
- `IPeriod=88000` — Z80 runs a full frame without interruption
- BG tile rendering: all 8 pixels per row written directly (no per-pixel branch)
- FG tile rendering: transparency check inlined, 8 pixels unrolled

---

## Source Files

| File | Purpose |
|---|---|
| `phoenix_emulator.cpp` | Main emulator: Z80 memory map, VBlank, renderer, setup/loop |
| `phoenix_audio.h` | Audio: LEDC PWM tone synthesis, boot sound |
| `phoenix_input.h` | Button reading, active-low IN0 byte |
| `phoenix_embedded_assets.h/.c` | ROM data baked into firmware (generated) |
| `make_embedded_roms.py` | ROM → C array converter with MAME-accurate palette bitswap |
| `Z80.c / Z80.h` | Z80 CPU core |
| `lcd_spi.cpp / lcd_spi.h` | ILI9341 SPI driver with DMA |
| `video_framebuffer.cpp` | Double-buffered 320×240 RGB565 framebuffer |
| `platformio.ini` | PlatformIO build configuration |

---

## Known Limitations

- Audio is single-channel (dominant effect wins): no true mixing
- MM6221AA melody chip is approximated with simple square waves
- Noise channel uses LFSR frequency modulation (not discrete circuit simulation)
- No cocktail cabinet mode (upright only)
- No high score save (no NVS/EEPROM integration)

---

## Credits

- Original game: **Phoenix** © 1980 Amstar Electronics / Centuri
- MAME source reference for hardware timing, palette, and GFX decode:  
  `mame_reference_phoenix.cpp`, `mame_video_phoenix_v.cpp`, `mame_phoenix_a.cpp`
- Z80 CPU core: based on fMSX Z80 emulator by Marat Fayzullin
- ESP32 implementation: custom, built iteratively with hardware feedback

---

## License

This project contains **no original ROM data**. ROM files must be sourced separately and are not redistributable. The emulator code is provided for educational purposes.
