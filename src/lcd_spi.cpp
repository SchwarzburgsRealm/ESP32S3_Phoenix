#include "lcd_spi.h"
#include "lcd.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG_LCD = "LCD_SPI";

static spi_device_handle_t s_spi = nullptr;
static spi_transaction_t s_pixel_trans[2];
static uint16_t *s_dma_chunk[2] = {nullptr, nullptr};
volatile uint32_t tft_frame_tx_count = 0u;

lcd_t lcd = {};

#define TFT_VIEW_W     320
#define TFT_VIEW_H     240

// No scaling tables needed for 320x240 direct display

#ifndef TFT_PIN_CS

#endif
#ifndef TFT_PIN_DC

#endif
#ifndef TFT_PIN_RST

#endif
#ifndef TFT_PIN_MOSI

#endif
#ifndef TFT_PIN_SCLK

#endif
#ifndef TFT_PIN_MISO

#endif

#define TFT_SPI_HOST    SPI2_HOST
#define TFT_SPI_FREQ_HZ (60 * 1000 * 1000)
#define TFT_PHYS_W      240
#define TFT_PHYS_H      320
#define TFT_DMA_CHUNK_LINES 16

static inline void dc_high(void) { gpio_set_level((gpio_num_t)TFT_PIN_DC, 1); }
static inline void dc_low(void)  { gpio_set_level((gpio_num_t)TFT_PIN_DC, 0); }

static void spi_pre_transfer_cb(spi_transaction_t *t) {
    const intptr_t dc = (intptr_t)t->user;
    gpio_set_level((gpio_num_t)TFT_PIN_DC, (uint32_t)dc ? 1 : 0);
}

static void tft_cmd(uint8_t cmd) {
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = (void *)0;
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_dat(uint8_t dat) {
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_data[0] = dat;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = (void *)1;
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_write_buf(const uint8_t *buf, size_t len) {
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = buf;
    t.user = (void *)1;
    spi_device_polling_transmit(s_spi, &t);
}

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    const uint8_t caset[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
    };
    const uint8_t paset[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
    };
    tft_cmd(0x2Au);
    tft_write_buf(caset, 4);
    tft_cmd(0x2Bu);
    tft_write_buf(paset, 4);
    tft_cmd(0x2Cu);
}

void tft_init(void) {
    ESP_LOGI(TAG_LCD, "Initializing TFT with pins: CS=%d, DC=%d, RST=%d, MOSI=%d, SCLK=%d, MISO=%d", 
             TFT_PIN_CS, TFT_PIN_DC, TFT_PIN_RST, TFT_PIN_MOSI, TFT_PIN_SCLK, TFT_PIN_MISO);
    
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << TFT_PIN_DC) | (1ULL << TFT_PIN_RST);
    gpio_config(&io_conf);

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = TFT_PIN_MOSI;
    buscfg.miso_io_num = TFT_PIN_MISO;
    buscfg.sclk_io_num = TFT_PIN_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = SCREEN_W * TFT_DMA_CHUNK_LINES * sizeof(uint16_t);

    esp_err_t err = spi_bus_initialize(TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_LCD, "spi_bus_initialize: %d", err);
        return;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = TFT_SPI_FREQ_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = TFT_PIN_CS;
    devcfg.queue_size = 2;
    devcfg.flags = 0;
    devcfg.pre_cb = spi_pre_transfer_cb;

    err = spi_bus_add_device(TFT_SPI_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_LCD, "spi_bus_add_device: %d", err);
        return;
    }

    gpio_set_level((gpio_num_t)TFT_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)TFT_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    tft_cmd(0x01u); vTaskDelay(pdMS_TO_TICKS(150));
    tft_cmd(0x11u); vTaskDelay(pdMS_TO_TICKS(120));

    tft_cmd(0xCFu); tft_dat(0x00u); tft_dat(0x83u); tft_dat(0x30u);
    tft_cmd(0xEDu); tft_dat(0x64u); tft_dat(0x03u); tft_dat(0x12u); tft_dat(0x81u);
    tft_cmd(0xE8u); tft_dat(0x85u); tft_dat(0x01u); tft_dat(0x79u);
    tft_cmd(0xCBu); tft_dat(0x39u); tft_dat(0x2Cu); tft_dat(0x00u); tft_dat(0x34u); tft_dat(0x02u);
    tft_cmd(0xF7u); tft_dat(0x20u);
    tft_cmd(0x3Au); tft_dat(0x55u);
    tft_cmd(0xC0u); tft_dat(0x23u);
    tft_cmd(0xC1u); tft_dat(0x11u);
    tft_cmd(0xC5u); tft_dat(0x31u); tft_dat(0x35u);
    tft_cmd(0xC7u); tft_dat(0x31u); tft_dat(0x35u);
    tft_cmd(0xEAu); tft_dat(0x02u); tft_dat(0x03u); tft_dat(0x0Bu); tft_dat(0x0Bu);
    tft_cmd(0x36u); tft_dat(0x28u); // MV=1 BGR=1
    tft_cmd(0xB1u); tft_dat(0x00u); tft_dat(0x1Bu);
    tft_cmd(0xB2u); tft_dat(0x0Cu); tft_dat(0x0Cu);
    tft_cmd(0xB6u); tft_dat(0x00u); tft_dat(0x82u); tft_dat(0x27u); tft_dat(0x00u);
    tft_cmd(0xF2u); tft_dat(0x00u);
    tft_cmd(0x26u); tft_dat(0x01u);
    tft_cmd(0xC7u); tft_dat(0xB4u);
    tft_cmd(0x2Au);
    tft_dat(0x00u); tft_dat(0x00u);
    tft_dat(0x00u); tft_dat(0xEFu);
    tft_cmd(0x2Bu);
    tft_dat(0x00u); tft_dat(0x00u);
    tft_dat(0x01u); tft_dat(0x3Fu);
    tft_cmd(0xE0u); tft_dat(0x0Fu); tft_dat(0x31u); tft_dat(0x2Bu); tft_dat(0x0Cu); tft_dat(0x0Eu); tft_dat(0x08u); tft_dat(0x4Eu);
    tft_dat(0xF1u); tft_dat(0x37u); tft_dat(0x07u); tft_dat(0x10u); tft_dat(0x03u); tft_dat(0x0Eu); tft_dat(0x09u); tft_dat(0x00u);
    tft_cmd(0xE1u); tft_dat(0x00u); tft_dat(0x0Eu); tft_dat(0x14u); tft_dat(0x03u); tft_dat(0x11u); tft_dat(0x07u); tft_dat(0x31u);
    tft_dat(0xC1u); tft_dat(0x48u); tft_dat(0x08u); tft_dat(0x0Fu); tft_dat(0x0Cu); tft_dat(0x31u); tft_dat(0x36u); tft_dat(0x0Fu);
    tft_cmd(0x11u); vTaskDelay(pdMS_TO_TICKS(120));
    tft_cmd(0x29u);

    // One-time clear so side borders outside the centered 267x240 window stay black
    {
        static uint16_t black_line[TFT_VIEW_W];
        memset(black_line, 0, sizeof(black_line));
        tft_set_window(0u, 0u, (uint16_t)(TFT_VIEW_W - 1u), (uint16_t)(TFT_VIEW_H - 1u));
        for (uint16_t y = 0; y < TFT_VIEW_H; ++y) {
            spi_transaction_t t = {};
            t.length = (uint32_t)TFT_VIEW_W * 16u;
            t.tx_buffer = black_line;
            t.user = (void *)1;
            spi_device_polling_transmit(s_spi, &t);
        }
    }

    for (uint32_t i = 0; i < 2; ++i) {
        s_dma_chunk[i] = static_cast<uint16_t *>(heap_caps_malloc(
            SCREEN_W * TFT_DMA_CHUNK_LINES * sizeof(uint16_t), MALLOC_CAP_DMA));
        if (!s_dma_chunk[i]) {
            ESP_LOGE(TAG_LCD, "heap_caps_malloc DMA chunk[%lu] failed", (unsigned long)i);
            return;
        }
        memset(&s_pixel_trans[i], 0, sizeof(s_pixel_trans[i]));
        s_pixel_trans[i].flags = 0;
        s_pixel_trans[i].tx_buffer = s_dma_chunk[i];
        s_pixel_trans[i].user = (void *)1;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG_LCD, "ILI9341 SPI DMA ready, %dx%d direct display @ %d MHz, queued ping-pong DMA",
             SCREEN_W, SCREEN_H, 60);
}

void tft_send_frame(const uint16_t *fb) {
    if (!s_spi || !s_dma_chunk[0] || !s_dma_chunk[1] || !fb) {
        return;
    }

    tft_set_window(0, 0, SCREEN_W - 1, SCREEN_H - 1);

    uint32_t queued = 0u;
    uint32_t buf_idx = 0u;

    for (uint16_t y = 0; y < SCREEN_H; y += TFT_DMA_CHUNK_LINES) {
        const uint16_t lines = (uint16_t)(((y + TFT_DMA_CHUNK_LINES) <= SCREEN_H)
            ? TFT_DMA_CHUNK_LINES
            : (SCREEN_H - y));
        const size_t chunk_pixels = (size_t)SCREEN_W * (size_t)lines;

        if (queued == 2u) {
            spi_transaction_t *done = nullptr;
            spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
            queued--;
        }

        uint16_t *dst = s_dma_chunk[buf_idx];
        const uint16_t *src = fb + y * SCREEN_W;
        for (size_t i = 0; i < chunk_pixels; ++i) {
            dst[i] = (src[i] << 8) | (src[i] >> 8);
        }

        spi_transaction_t *t = &s_pixel_trans[buf_idx];
        t->length = (uint32_t)(chunk_pixels * 16u);
        t->rxlength = 0u;
        t->tx_buffer = dst;

        const esp_err_t err = spi_device_queue_trans(s_spi, t, portMAX_DELAY);
        if (err != ESP_OK) {
            while (queued > 0u) {
                spi_transaction_t *done = nullptr;
                spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
                queued--;
            }
            return;
        }

        queued++;
        buf_idx ^= 1u;
    }

    while (queued > 0u) {
        spi_transaction_t *done = nullptr;
        spi_device_get_trans_result(s_spi, &done, portMAX_DELAY);
        queued--;
    }

    tft_frame_tx_count++;
}

void tft_wait_frame(void) {
}

void test_framebuffer_content(void) {
    ESP_LOGI(TAG_LCD, "=== FRAMEBUFFER CONTENT TEST ====");

    for (uint16_t y = 0; y < SCREEN_H; y++) {
        uint16_t *line = &lcd.fb[(uint16_t)y * SCREEN_W];
        for (uint16_t x = 0; x < SCREEN_W; x++) {
            if (y < 20) {
                line[x] = 0xF800;
            } else if (y < 40) {
                line[x] = 0x07E0;
            } else if (y < 60) {
                line[x] = 0x001F;
            } else {
                line[x] = ((y + x) % 16 < 8) ? 0x0000 : 0xFFFF;
            }
        }
    }

    const uint8_t test_font[4][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
        {0x7E, 0x40, 0x38, 0x40, 0x7E},
        {0x7E, 0x40, 0x38, 0x40, 0x7E},
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
    };

    for (int char_idx = 0; char_idx < 4; char_idx++) {
        for (int col = 0; col < 5; col++) {
            uint8_t bits = test_font[char_idx][col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    int px = 10 + char_idx * 6 + col;
                    int py = 70 + row;
                    if (px < SCREEN_W && py < SCREEN_H) {
                        lcd.fb[py * SCREEN_W + px] = 0xFFFF;
                    }
                }
            }
        }
    }

    lcd.frame_ready = true;
}
