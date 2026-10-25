// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <vector>

namespace ttrally::cutting {

/// A rally in seconds on the video's timeline, end exclusive.
struct TimeSpan {
    double start_s = 0.0;
    double end_s = 0.0;
};

/// Time added before and after every rally.
struct Margins {
    double before_s = 1.0;
    double after_s = 1.0;
};

/// A part of the video that goes into the cut, end exclusive; one or more rallies whose margins
/// overlap.
struct CutSection {
    double start_s = 0.0;
    double end_s = 0.0;
    std::size_t rallies = 0;

    [[nodiscard]] double duration_s() const noexcept { return end_s - start_s; }
};

/// Sections to keep: every rally extended by the margins, limited to the video
/// [video_start_s, video_end_s), and merged with its neighbour where the extended spans overlap
/// or touch (nothing is shown twice). Throws std::invalid_argument for negative margins.
[[nodiscard]] std::vector<CutSection> plan_cut(std::vector<TimeSpan> rallies, Margins margins,
                                               double video_start_s, double video_end_s);

/// Total duration of the sections.
[[nodiscard]] double total_duration_s(const std::vector<CutSection>& sections) noexcept;

} // namespace ttrally::cutting
