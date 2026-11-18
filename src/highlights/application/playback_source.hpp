// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/frame_decoder.hpp"

#include <filesystem>
#include <memory>

namespace ttrally::highlights {

/// Port: opens a video to watch rallies in the launcher, numbered like the labels.
class PlaybackSource {
  public:
    virtual ~PlaybackSource() = default;
    /// Frames scaled to `height` pixels; the frame timestamps are cached in `cache_dir`.
    /// Throws media::MediaError if the video cannot be read.
    [[nodiscard]] virtual std::unique_ptr<media::FrameDecoder>
    open(const std::filesystem::path& video, const std::filesystem::path& cache_dir,
         int height) = 0;
};

} // namespace ttrally::highlights
