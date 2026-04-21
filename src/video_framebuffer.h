#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

class VideoFramebuffer {
public:
    static constexpr uint16_t kWidth = 320u;
    static constexpr uint16_t kHeight = 240u;
    static constexpr size_t kPixels = static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight);
    static constexpr size_t kBytes = kPixels * sizeof(uint16_t);

    bool begin();
    uint16_t* begin_write();
    void cancel_write();
    void publish_write(uint16_t width, uint16_t height);

    const uint16_t* front_buffer() const { return front_; }
    bool consume_ready(uint16_t& width, uint16_t& height);
    bool ready() const { return ready_; }
    bool is_busy() const { return writing_; }

private:
    uint16_t* front_ = nullptr;
    uint16_t* back_ = nullptr;
    bool writing_ = false;
    volatile bool ready_ = false;
    volatile uint16_t published_w_ = 0;
    volatile uint16_t published_h_ = 0;
};
