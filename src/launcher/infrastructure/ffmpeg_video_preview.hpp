// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/video_preview.hpp"

namespace ttrally::launcher {

/// Decodes a preview frame with FFmpeg: seeks to the time, decodes on the CPU up to the frame
/// shown there and scales it down. Takes a fraction of a second for camera footage.
class FfmpegVideoPreview final : public VideoPreview {
  public:
    [[nodiscard]] std::optional<media::VideoFrame>
    frame_at(const std::filesystem::path& video, double fraction, int height) override;
};

} // namespace ttrally::launcher
