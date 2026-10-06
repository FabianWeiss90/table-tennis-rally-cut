// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/kernel/frame_timeline.hpp"
#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <vector>

namespace ttrally::media {

/// Presentation timestamps of all frames of a video stream, sorted ascending.
struct VideoTimestamps {
    std::vector<std::int64_t> pts;
    Rational time_base;

    [[nodiscard]] std::int64_t frame_count() const noexcept {
        return static_cast<std::int64_t>(pts.size());
    }

    /// Time-to-frame mapping; constant frame rate if the PTS follow the nominal grid.
    [[nodiscard]] FrameTimeline timeline(Rational nominal_fps) const {
        return FrameTimeline::from_pts(pts, time_base, nominal_fps);
    }
};

} // namespace ttrally::media
