// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/video_frame.hpp"

#include <cstdint>

struct SDL_Renderer;
struct SDL_Texture;

namespace ttrally::gui {

/// GPU texture showing one decoded video frame (YUV 4:2:0, converted by the renderer).
class FrameTexture {
  public:
    explicit FrameTexture(SDL_Renderer* renderer) : renderer_(renderer) {}
    ~FrameTexture();
    FrameTexture(const FrameTexture&) = delete;
    FrameTexture& operator=(const FrameTexture&) = delete;

    /// Uploads the frame unless it is already shown.
    void show(const media::VideoFrame& frame);

    [[nodiscard]] SDL_Texture* texture() const noexcept { return texture_; }
    [[nodiscard]] media::FrameSize size() const noexcept { return size_; }
    [[nodiscard]] std::int64_t frame_index() const noexcept { return index_; }

  private:
    SDL_Renderer* renderer_;
    SDL_Texture* texture_ = nullptr;
    media::FrameSize size_;
    std::int64_t index_ = -1;
};

} // namespace ttrally::gui
