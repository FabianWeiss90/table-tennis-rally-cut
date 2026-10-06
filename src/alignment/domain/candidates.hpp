// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>
#include <optional>

namespace ttrally::alignment {

/// A segment of the cut video located in the original: a candidate for manual annotation.
/// Times are presentation times in seconds on each file's own timeline.
struct CandidateSegment {
    int id = 0; ///< 1-based, in cut order
    double cut_start_s = 0.0;
    double cut_end_s = 0.0;
    double orig_start_s = 0.0;
    double orig_end_s = 0.0;
    std::int64_t orig_start_frame = 0;
    std::int64_t orig_end_frame = 0; ///< inclusive
    double offset_s = 0.0;           ///< orig_start_s - cut_start_s
    double confidence = 0.0;
    std::size_t window_count = 0;
    std::optional<double> visual_similarity; ///< Result of the visual spot check, if done

    [[nodiscard]] double duration_s() const { return cut_end_s - cut_start_s; }
    [[nodiscard]] double cut_middle_s() const { return 0.5 * (cut_start_s + cut_end_s); }
};

/// Part of the original not covered by any candidate: may contain missed rallies.
struct Gap {
    int id = 0;
    int after_candidate = 0; ///< 0 = before the first candidate
    double orig_start_s = 0.0;
    double orig_end_s = 0.0;
    std::int64_t orig_start_frame = 0;
    std::int64_t orig_end_frame = 0; ///< inclusive

    [[nodiscard]] double duration_s() const { return orig_end_s - orig_start_s; }
};

} // namespace ttrally::alignment
