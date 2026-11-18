// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/cut_plan.hpp"
#include "shared/kernel/frame_timeline.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ttrally::cutting {

/// A rally by its frames in the original, end inclusive.
struct FrameSpan {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
};

/// Sections of a video with `frame_count` frames for the rallies: each extended by the margins,
/// merged where they overlap (see plan_cut), with boundaries on frame times so that the sections
/// follow each other without gaps. Throws std::invalid_argument for a rally outside the video or
/// negative margins.
[[nodiscard]] std::vector<CutSection> plan_sections(std::span<const FrameSpan> rallies,
                                                    Margins margins,
                                                    const FrameTimeline& timeline,
                                                    std::int64_t frame_count);

} // namespace ttrally::cutting
