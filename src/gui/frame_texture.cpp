// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/frame_texture.hpp"

#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>

namespace ttrally::gui {

FrameTexture::~FrameTexture() {
    if (texture_ != nullptr) {
        SDL_DestroyTexture(texture_);
    }
}

void FrameTexture::show(const media::VideoFrame& frame) {
    if (frame.index == index_ && texture_ != nullptr) {
        return;
    }
    if (texture_ == nullptr || frame.size.width != size_.width ||
        frame.size.height != size_.height) {
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
        }
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
                                     SDL_TEXTUREACCESS_STREAMING, frame.size.width,
                                     frame.size.height);
        if (texture_ == nullptr) {
            throw std::runtime_error(std::string("cannot create video texture: ") + SDL_GetError());
        }
        size_ = frame.size;
    }
    SDL_UpdateYUVTexture(texture_, nullptr, frame.y().data(), frame.size.width, frame.u().data(),
                         frame.chroma_width(), frame.v().data(), frame.chroma_width());
    index_ = frame.index;
}

} // namespace ttrally::gui
