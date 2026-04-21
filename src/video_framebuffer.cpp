#include "video_framebuffer.h"

#include <string.h>

bool VideoFramebuffer::begin() {
    front_ = static_cast<uint16_t*>(ps_malloc(kBytes));
    back_ = static_cast<uint16_t*>(ps_malloc(kBytes));
    if (front_ == nullptr || back_ == nullptr) {
        return false;
    }
    memset(front_, 0, kBytes);
    memset(back_, 0, kBytes);
    writing_ = false;
    ready_ = false;
    published_w_ = 0;
    published_h_ = 0;
    return true;
}

uint16_t* VideoFramebuffer::begin_write() {
    if (front_ == nullptr || back_ == nullptr || writing_) {
        return nullptr;
    }
    writing_ = true;
    return back_;
}

void VideoFramebuffer::cancel_write() {
    writing_ = false;
}

void VideoFramebuffer::publish_write(uint16_t width, uint16_t height) {
    if (!writing_) {
        return;
    }

    noInterrupts();
    uint16_t* tmp = front_;
    front_ = back_;
    back_ = tmp;
    published_w_ = width;
    published_h_ = height;
    ready_ = true;
    interrupts();

    writing_ = false;
}

bool VideoFramebuffer::consume_ready(uint16_t& width, uint16_t& height) {
    if (!ready_) {
        return false;
    }

    noInterrupts();
    width = published_w_;
    height = published_h_;
    ready_ = false;
    interrupts();
    return true;
}
