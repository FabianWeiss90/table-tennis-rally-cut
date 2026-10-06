// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ttrally::media {

struct VideoStreamInfo {
    int index = -1;
    std::string codec;
    int width = 0;
    int height = 0;
    Rational time_base;
    Rational avg_frame_rate;
    Rational r_frame_rate;
    double start_time_s = 0.0; ///< Presentation time of the first frame
    double duration_s = 0.0;   ///< NaN if unknown

    /// avg_frame_rate, falling back to r_frame_rate.
    [[nodiscard]] Rational nominal_frame_rate() const noexcept {
        return avg_frame_rate.positive() ? avg_frame_rate : r_frame_rate;
    }
};

struct AudioStreamInfo {
    int index = -1;
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
    Rational time_base;
    double start_time_s = 0.0;
    double duration_s = 0.0; ///< NaN if unknown
};

/// Container and stream properties of a media file.
struct MediaInfo {
    std::filesystem::path path;
    std::uintmax_t file_size = 0;
    std::string container;
    double duration_s = 0.0; ///< NaN if unknown
    std::optional<VideoStreamInfo> video;
    std::optional<AudioStreamInfo> audio;
};

} // namespace ttrally::media
