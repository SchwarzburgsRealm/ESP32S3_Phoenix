#include <Arduino.h>
#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// LCD Driver
#include "lcd_spi.h"
#include "video_framebuffer.h"
#include "phoenix_audio.h"
#include "phoenix_input.h"

// Undefine Arduino's word macro to avoid conflict with Z80.h
#undef word

// Include Z80 core AFTER Arduino includes
extern "C" {
#include "Z80.h"
#include "phoenix_embedded_assets.h"
}

static const char* TAG = "PHOENIX";

// === Z80 MEMORY MAP ===
// 0000-3FFF: ROM (16KB)
// 4000-4FFF: Video RAM Bank 0/1 (4KB, banked)
// 5000-53FF: Video registers
// 5800-5BFF: Scroll register
// 6000-63FF: Sound A
// 6800-6BFF: Sound B
// 7000-73FF: Input 0
// 7800-7BFF: DIP switches

// Memory
static uint8_t video_ram[2][0x1000];  // Two 4KB banks
static uint8_t bank_select = 0;        // Current video bank
static uint8_t video_reg = 0;          // Video register at 0x5000
static uint8_t scroll_reg = 0;         // Scroll register at 0x5800
static uint8_t sound_a = 0;            // Sound control A
static uint8_t sound_b = 0;            // Sound control B
static uint8_t input_0 = 0xFF;         // Input port (buttons)
static uint8_t dip_sw = 0xFF;          // DIP switches

// Z80 CPU state
static Z80 cpu;

// Display
static VideoFramebuffer g_fb;

// Tile graphics (8x8 pixels, from ROM)
static const uint8_t* fg_tiles = phoenix_fg_tiles;
static const uint8_t* bg_tiles = phoenix_bg_tiles;

// === PHOENIX DISPLAY GEOMETRY (from MAME source) ===
// Native hardware visible area: 256 wide x 208 tall (landscape, 32 cols x 26 rows of 8x8 tiles)
// Tilemap is 32x32 but only 26 rows are visible (VBSTART=208 in MAME).
// LCD is landscape 320x240 (MADCTL=0x20). Native image 256x208 fits inside, centred:
//   horizontal border = (320-256)/2 = 32 px left and right
//   vertical border   = (240-208)/2 = 16 px top and bottom
// No rotation: native pixel (nx, ny) -> LCD pixel (nx + BORDER_X, ny + BORDER_Y)
#define TILE_SIZE       8
#define TILE_COLS_VIS   32   // 32 tile columns = 256 px native width
#define TILE_ROWS_VIS   26   // 26 tile rows    = 208 px native height (visible)
#define NATIVE_W        256  // native pixel width
#define NATIVE_H        208  // native pixel height
#define LCD_W           320  // LCD width  (landscape)
#define LCD_H           240  // LCD height (landscape)
#define BORDER_X        32   // (LCD_W - NATIVE_W) / 2 = (320-256)/2
#define BORDER_Y        16   // (LCD_H - NATIVE_H) / 2 = (240-208)/2

// === Z80 MEMORY ACCESS ===
// Phoenix uses memory-mapped I/O - no IN/OUT instructions!

// VBLANK timing - DIP bit 7 toggles at 60Hz
static uint8_t vblank_flag = 0x80;  // Bit 7 of DIP register
static uint32_t last_vblank_toggle = 0;

// Precomputed vblank: toggled by cycle count, no micros() in hot loop
static uint8_t  s_vblank_state  = 0x80;  // bit7: HIGH=active, LOW=blanking
static uint32_t s_vblank_cycles = 0;     // unused, kept for reference

extern "C" IRAM_ATTR byte RdZ80(uint16_t Addr) {
    // ROM is ~80% of all reads (opcode fetch every instruction) -> check FIRST
    if (Addr < 0x4000)
        return phoenix_prog_rom[Addr];

    // VRAM (video ops, moderate frequency)
    if (Addr < 0x5000)
        return video_ram[bank_select][Addr - 0x4000];

    // DSW0 + VBlank bit7 (tight loop in WaitVBlankCoin)
    // Use cycle counter instead of micros() - no system call overhead
    if (Addr < 0x7C00) {
        if (Addr >= 0x7800)
            return dip_sw | s_vblank_state;
        // IN0 buttons (0x7000-0x73FF)
        if (Addr >= 0x7000)
            return input_0;
    }
    return 0x00;
}

extern "C" IRAM_ATTR void WrZ80(uint16_t Addr, byte Value) {
    // VRAM is most frequent write (tile updates every frame)
    if (Addr >= 0x4000 && Addr < 0x5000) {
        video_ram[bank_select][Addr - 0x4000] = Value;
        return;
    }
    if (Addr >= 0x5000 && Addr < 0x5400) {
        video_reg = Value;
        bank_select = Value & 0x01;
    } else if (Addr >= 0x5800 && Addr < 0x5C00) {
        scroll_reg = Value;
    } else if (Addr >= 0x6000 && Addr < 0x6400) {
        sound_a = Value;
        phoenix_audio_control_a(Value);
    } else if (Addr >= 0x6800 && Addr < 0x6C00) {
        sound_b = Value;
        phoenix_audio_control_b(Value);
    }
}

// Stub I/O functions (not used by Phoenix - it uses memory-mapped I/O)
extern "C" byte InZ80(uint16_t Port) { return 0xFF; }
extern "C" void OutZ80(uint16_t Port, byte Value) { (void)Port; (void)Value; }

// Forward declaration for video rendering
static IRAM_ATTR void render_screen();

// === Z80 INTERRUPT HANDLING ===

extern "C" IRAM_ATTR uint16_t LoopZ80(Z80 *R) {
    // Original Phoenix: 8085 @ 2.75MHz, 60Hz = 45833 cycles/frame
    // Phase 0: 40000 cycles active display (bit7=HIGH)
    // Phase 1:  5833 cycles vblank        (bit7=LOW) -> then render
    static uint8_t phase = 0;
    if (phase == 0) {
        s_vblank_state = 0x00;   // enter vblank
        cpu.IPeriod = 5833;
        phase = 1;
        return INT_NONE;
    }
    s_vblank_state = 0x80;       // back to active display
    cpu.IPeriod = 40000;
    phase = 0;

    input_0 = phoenix_read_inputs();
    phoenix_audio_update();
    render_screen();
    return INT_QUIT;
}

extern "C" void PatchZ80(Z80 *R) {
    // No patches needed
}

// Forward declaration
static IRAM_ATTR void render_screen();

// === VIDEO RENDERING ===
//
// Tile GFX ROM layout (from MAME charlayout):
//   256 tiles, 2 bits per pixel, 8x8 pixels
//   Each tile = 8 bytes plane0 + 8 bytes plane1 = 16 bytes total
//   Plane0 starts at offset 0, Plane1 starts at offset 0x800 within the ROM
//   So: tile N plane0 row R = tiles[N*8 + R]
//       tile N plane1 row R = tiles[0x800 + N*8 + R]
//   Pixel bit order: bit7 = leftmost pixel (col 0), bit0 = rightmost (col 7)
//   2bpp pixel value: (plane1_bit << 1) | plane0_bit
//
// Tilemap VRAM layout (MAME: 32 cols x 32 rows, TILEMAP_SCAN_ROWS):
//   FG tilemap: vram[0x0000 .. 0x03FF]  row-major, row*32+col
//   BG tilemap: vram[0x0800 .. 0x0BFF]  row-major, row*32+col
//   Visible:    32 cols x 26 rows = 256x208 pixels (native landscape)
//
// Palette (from MAME get_fg/bg_tile_info):
//   tile byte = code (all 8 bits used for tile select)
//   color_attr = code >> 5          (upper 3 bits = color 0-7)
//   FG palette entry = (palette_bank << 4) | 0x08 | color_attr
//   BG palette entry = (palette_bank << 4) | 0x00 | color_attr
//   Final palette index = entry * 4 + pixel_value  (pixel 0-3)
//   palette565[] has 256 entries covering all combinations.
//
// LCD orientation: landscape 320x240 (MADCTL=0x20).
//   Native image 256x208 centred: border X=32, border Y=16.
//   Direct mapping: native(nx,ny) -> lcd(nx+BORDER_X, ny+BORDER_Y)

// Precomputed tile row: 8 pixels decoded in one shot, written as row to fb
static IRAM_ATTR void render_tile(uint16_t* fb,
                        int tile_col, int tile_row,
                        uint8_t code,
                        const uint8_t* tile_rom,
                        uint8_t palette_bank,
                        bool is_fg)
{
    uint8_t color_attr = code >> 5;
    uint8_t layer_base = is_fg ? 8 : 0;
    int pal_group = ((palette_bank << 4) | layer_base | color_attr) * 4;

    const uint8_t* p0 = tile_rom + ((int)code * 8);
    const uint8_t* p1 = tile_rom + 0x800 + ((int)code * 8);

    // Base LCD address for this tile (top-left corner)
    const int base_x = tile_col * 8 + BORDER_X;
    const int base_y = tile_row * 8 + BORDER_Y;

    for (int row = 7; row >= 0; row--) {
        uint8_t b0 = p0[row];
        uint8_t b1 = p1[row];
        uint16_t* line = fb + (base_y + row) * LCD_W + base_x;

        // Decode all 8 pixels of this row at once — no per-pixel bounds check needed
        // (tile is always fully inside the framebuffer by construction)
        if (!is_fg) {
            // BG: all pixels opaque, write all 8 directly
            line[7] = phoenix_palette565[pal_group + (((b1>>7)&1)<<1)|((b0>>7)&1)];
            line[6] = phoenix_palette565[pal_group + (((b1>>6)&1)<<1)|((b0>>6)&1)];
            line[5] = phoenix_palette565[pal_group + (((b1>>5)&1)<<1)|((b0>>5)&1)];
            line[4] = phoenix_palette565[pal_group + (((b1>>4)&1)<<1)|((b0>>4)&1)];
            line[3] = phoenix_palette565[pal_group + (((b1>>3)&1)<<1)|((b0>>3)&1)];
            line[2] = phoenix_palette565[pal_group + (((b1>>2)&1)<<1)|((b0>>2)&1)];
            line[1] = phoenix_palette565[pal_group + (((b1>>1)&1)<<1)|((b0>>1)&1)];
            line[0] = phoenix_palette565[pal_group + (((b1>>0)&1)<<1)|((b0>>0)&1)];
        } else {
            // FG: pixel 0 is transparent, check each
            uint8_t px;
            px=(((b1>>7)&1)<<1)|((b0>>7)&1); if(px) line[7]=phoenix_palette565[pal_group+px];
            px=(((b1>>6)&1)<<1)|((b0>>6)&1); if(px) line[6]=phoenix_palette565[pal_group+px];
            px=(((b1>>5)&1)<<1)|((b0>>5)&1); if(px) line[5]=phoenix_palette565[pal_group+px];
            px=(((b1>>4)&1)<<1)|((b0>>4)&1); if(px) line[4]=phoenix_palette565[pal_group+px];
            px=(((b1>>3)&1)<<1)|((b0>>3)&1); if(px) line[3]=phoenix_palette565[pal_group+px];
            px=(((b1>>2)&1)<<1)|((b0>>2)&1); if(px) line[2]=phoenix_palette565[pal_group+px];
            px=(((b1>>1)&1)<<1)|((b0>>1)&1); if(px) line[1]=phoenix_palette565[pal_group+px];
            px=(((b1>>0)&1)<<1)|((b0>>0)&1); if(px) line[0]=phoenix_palette565[pal_group+px];
        }
    }
}

static IRAM_ATTR void render_tile_scrolled(uint16_t* fb,
                                  int tile_col, int tile_row,
                                  uint8_t code,
                                  const uint8_t* tile_rom,
                                  uint8_t palette_bank,
                                  bool is_fg,
                                  uint8_t scroll_offset)
{
    uint8_t color_attr = code >> 5;
    int pal_group = ((palette_bank << 4) | color_attr) * 4;

    const uint8_t* p0 = tile_rom + ((int)code * 8);
    const uint8_t* p1 = tile_rom + 0x800 + ((int)code * 8);

    for (int row = 7; row >= 0; row--) {
        uint8_t b0 = p0[row];
        uint8_t b1 = p1[row];
        int ny = tile_row * 8 + row + BORDER_Y;
        if ((unsigned)ny >= LCD_H) continue;

        for (int col = 7; col >= 0; col--) {
            uint8_t pixel = (((b1 >> col) & 1) << 1) | ((b0 >> col) & 1);
            if (pixel == 0) continue;
            int nx = ((tile_col * 8 + col + (NATIVE_W - scroll_offset)) & (NATIVE_W - 1)) + BORDER_X;
            if ((unsigned)nx < LCD_W)
                fb[ny * LCD_W + nx] = phoenix_palette565[pal_group + pixel];
        }
    }
}

// Forward declaration — defined after setup()
static TaskHandle_t s_spi_task_handle  = nullptr;
static TaskHandle_t s_main_task_handle = nullptr;  // Core1 main task

static IRAM_ATTR void render_screen() {
    uint16_t* fb = g_fb.begin_write();
    if (!fb) return;

    // Clear framebuffer to black (borders will stay black)
    memset(fb, 0, VideoFramebuffer::kBytes);

    // Active VRAM bank (bank_select toggled by video_reg bit 0)
    uint8_t* vram = video_ram[bank_select];

    // Palette bank from video_reg bit 1 (MAME: (data>>1)&1)
    uint8_t palette_bank = (video_reg >> 1) & 0x01;

    // --- Background layer (BG tilemap at vram[0x0800], row-major 32 cols x 32 rows) ---
    // BG scrollt horizontal pixel-genau via scroll_reg (0x5800 write).
    // MAME: m_bg_tilemap->set_scrollx(0, data)
    // Wir rendern alle 32 Tile-Spalten mit Pixel-Offset und wrappen bei 256px.
    for (int row = 0; row < TILE_ROWS_VIS; row++) {
        for (int col = 0; col < TILE_COLS_VIS; col++) {
            uint8_t code = vram[0x0800 + row * 32 + col];
            render_tile_scrolled(fb, col, row, code, bg_tiles, palette_bank, false, scroll_reg);
        }
    }

    // --- Foreground layer (FG tilemap at vram[0x0000], row-major 32 cols x 32 rows) ---
    for (int row = 0; row < TILE_ROWS_VIS; row++) {
        for (int col = 0; col < TILE_COLS_VIS; col++) {
            uint8_t code = vram[row * 32 + col];
            render_tile(fb, col, row, code, fg_tiles, palette_bank, true);
        }
    }

    g_fb.publish_write(VideoFramebuffer::kWidth, VideoFramebuffer::kHeight);
    // Signal Core0 to send frame, then WAIT until it's done before returning.
    // This prevents Core1 from overwriting the buffer while Core0 is sending it.
    xTaskNotifyGive(s_spi_task_handle);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for SPI done signal back
}

// === EMULATOR CONTROL ===

static void z80_reset() {
    memset(&cpu, 0, sizeof(cpu));
    // Phoenix uses 8085 @ 5.5MHz (5500000 cycles/sec)
    // At 60Hz, that's ~91667 cycles per frame
    // Call LoopZ80 every ~1000 cycles for vblank polling
    // IPeriod = voller Frame auf einmal: 88000 Zyklen @ 5.5MHz = ~16ms
    // LoopZ80 wird nur 1x pro Frame aufgerufen -> minimaler Overhead
    // cpu.IPeriod = 40000;  // active display phase (2.75MHz / 60Hz * 86%)
    cpu.IPeriod = 46000;  // active display phase (2.75MHz / 60Hz * 86%)
    // Note: memset(0) sets IFF1=0, IFF2=0 (interrupts disabled)
    // Phoenix uses SEI/DI - no IRQ ever! It polls DIP bit 7 for vblank.
    // Initialize stack pointer to top of RAM (0x4BF8 as per ClearRAMBank)
    cpu.SP.W = 0x4BF8;
    ResetZ80(&cpu);
}







// === CORE SPLIT ===
// Core 1: Z80 emulation + tile rendering (time critical)
// Core 0: SPI frame transfer to LCD (runs in parallel with next frame render)
static void spi_task(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for frame ready
        tft_send_frame(g_fb.front_buffer());       // send to LCD
        xTaskNotifyGive(s_main_task_handle);       // signal: done, Core1 can render next
    }
}

// === ARDUINO SETUP/LOOP ===

void setup() {
    // Serial init with minimal delay
    Serial.begin(115200);
    delay(100);  // Reduced from 1200
    
    
    // Initialize TFT display
    tft_init();

    // Initialize framebuffer
    if (!g_fb.begin()) {
        Serial.println("[ERROR] framebuffer alloc failed");
        for (;;) delay(1000);
    }

    // Initialize emulator
    memset(video_ram, 0, sizeof(video_ram));
    
    // Set defaults
    // Bit 7 of DSW0 is the VBLANK hardware signal, NOT a DIP bit.
    // Must be 0 here so vblank_state can drive bit 7 properly in RdZ80().
    dip_sw = 0x40;
    input_0 = 0xFF;
    bank_select = 0;
    video_reg = 0;
    
    // Reset Z80 to address 0x0000
    phoenix_audio_init();
    phoenix_input_init();
    phoenix_boot_sound();

    // SPI transfer task on Core 0, Core 1 runs Z80+render
    // Capture main task handle so spi_task can signal back
    s_main_task_handle = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(spi_task, "spi", 2048, nullptr, 10, &s_spi_task_handle, 0);

    z80_reset();
}

void loop() {
    // Run Z80 CPU - RunZ80 returns when LoopZ80 returns INT_QUIT
    uint16_t pc = RunZ80(&cpu);
    
}