// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/cut_plan.hpp"
#include "shared/application/progress_reporter.hpp"

#include <filesystem>
#include <span>
#include <string>

namespace ttrally::cutting {

enum class CutMode {
    Exact, ///< Re-encodes; cuts exactly at the section times
    Fast,  ///< Copies the compressed video; sections are widened to the surrounding keyframes
};

/// Video format of re-encoded cuts.
enum class VideoCodec {
    H265, ///< HEVC: much better quality per bit; plays in current players
    H264, ///< AVC: plays everywhere
};

struct CutOptions {
    CutMode mode = CutMode::Exact;
    VideoCodec codec = VideoCodec::H265;
    std::string encoder = "auto";   ///< Video encoder for Exact; "auto" tries GPU encoders first
    /// Video bit rate for Exact; 0 = by codec, resolution and frame rate (H.265: 5 Mbit/s for
    /// Full HD at 60 fps, H.264: twice that)
    double bitrate_mbps = 0.0;
};

struct CutResult {
    std::string video_encoder; ///< Encoder used, or "copy"
    double bitrate_mbps = 0.0; ///< Requested video bit rate (0 for copies)
    double duration_s = 0.0;   ///< Duration of the written video
};

/// Port: writes the given sections of a video, one after the other, into a new video file.
class VideoCutter {
  public:
    virtual ~VideoCutter() = default;
    /// Sections are sorted and do not overlap. Throws media::MediaError.
    [[nodiscard]] virtual CutResult cut(const std::filesystem::path& input,
                                        std::span<const CutSection> sections,
                                        const std::filesystem::path& output,
                                        const CutOptions& options, ProgressReporter& progress) = 0;
};

} // namespace ttrally::cutting
