// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/playback_source.hpp"

namespace ttrally::highlights {

/// Opens videos with the FFmpeg frame decoder (GPU decoding if available) and the cached frame
/// timestamps.
class FfmpegPlaybackSource final : public PlaybackSource {
  public:
    [[nodiscard]] std::unique_ptr<media::FrameDecoder>
    open(const std::filesystem::path& video, const std::filesystem::path& cache_dir,
         int height) override;
};

} // namespace ttrally::highlights
