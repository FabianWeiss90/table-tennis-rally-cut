// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/video_frame.hpp"

#include <filesystem>
#include <optional>

namespace ttrally::launcher {

/// Port: single frames of a video for the preview in the launcher.
class VideoPreview {
  public:
    virtual ~VideoPreview() = default;
    /// The frame shown at `fraction` (0 to 1) of the video's duration, scaled to about `height`
    /// pixels (YUV 4:2:0); nullopt if the video cannot be read.
    [[nodiscard]] virtual std::optional<media::VideoFrame>
    frame_at(const std::filesystem::path& video, double fraction, int height) = 0;
};

} // namespace ttrally::launcher
