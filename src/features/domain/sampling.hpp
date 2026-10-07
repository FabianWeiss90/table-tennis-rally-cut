// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/kernel/frame_timeline.hpp"

#include <cstdint>
#include <vector>

namespace ttrally::features {

/// One point of the regular time grid at which features are computed.
struct Sample {
    double time_s = 0.0;     ///< Grid time on the video's timeline
    std::int64_t frame = 0;  ///< Frame shown at that time (nearest frame)
};

/// Samples every 1/rate_hz seconds from the first to the last frame. The grid is defined in
/// time, so it is independent of the video's frame rate; each sample uses the nearest frame.
[[nodiscard]] std::vector<Sample> plan_samples(const FrameTimeline& timeline,
                                               std::int64_t frame_count, double rate_hz);

/// The distinct frames of the samples, in ascending order (what has to be decoded).
[[nodiscard]] std::vector<std::int64_t> frames_to_decode(const std::vector<Sample>& samples);

} // namespace ttrally::features
