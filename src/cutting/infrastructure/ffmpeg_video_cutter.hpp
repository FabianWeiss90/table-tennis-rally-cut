// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/video_cutter.hpp"

#include <string>
#include <vector>

namespace ttrally::cutting {

/// Cuts with the FFmpeg libraries into an MP4 file (H.265 or H.264 video, AAC audio).
///
/// Exact: reads the video once from start to end, keeps the decoded frames and audio samples
/// inside the sections and encodes them; the video with the first encoder of the chosen codec
/// that opens (GPU encoders first; H.265 falls back to H.264; then OpenH264, then FFmpeg's
/// MPEG-4 encoder; never GPL code such as x264 or x265).
/// Fast: copies the compressed packets; every section is widened to the keyframes around it, so
/// the cuts are up to a few seconds wider than asked.
class FfmpegVideoCutter final : public VideoCutter {
  public:
    [[nodiscard]] CutResult cut(const std::filesystem::path& input,
                                std::span<const CutSection> sections,
                                const std::filesystem::path& output, const CutOptions& options,
                                ProgressReporter& progress) override;
};

/// Video encoders tried by `auto` for a codec, in this order (only those FFmpeg was built with).
[[nodiscard]] std::vector<std::string> automatic_video_encoders(VideoCodec codec);

} // namespace ttrally::cutting
