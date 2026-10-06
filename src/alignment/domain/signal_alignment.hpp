// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ttrally::alignment {

/// Match of one window of the cut audio in the original audio.
struct WindowMatch {
    std::size_t cut_index = 0;  ///< First sample of the window in the cut signal
    std::ptrdiff_t offset = 0;  ///< original index - cut index (samples)
    float peak = 0.0F;          ///< Correlation at the best match
    float confidence = 0.0F;    ///< Peak ratio (see MatchResult)
    bool valid = false;         ///< A match could be computed (window not silent)
    bool confident = false;     ///< Meets min_confidence and min_peak
    bool global_search = false; ///< The whole original was searched
};

/// Contiguous piece of the cut audio that maps to the original with a constant offset.
struct MatchedSegment {
    std::size_t cut_start = 0; ///< First sample (inclusive)
    std::size_t cut_end = 0;   ///< Last sample (exclusive)
    std::ptrdiff_t offset = 0; ///< original index - cut index (median over the windows)
    float confidence = 0.0F;   ///< Median window confidence
    std::size_t first_window = 0;
    std::size_t last_window = 0;
    std::size_t window_count = 0;

    [[nodiscard]] std::ptrdiff_t original_start() const {
        return static_cast<std::ptrdiff_t>(cut_start) + offset;
    }
    [[nodiscard]] std::ptrdiff_t original_end() const {
        return static_cast<std::ptrdiff_t>(cut_end) + offset;
    }
};

/// Result of aligning two audio signals, in samples.
struct SignalAlignment {
    std::size_t window_length = 0;
    std::vector<WindowMatch> windows;
    std::vector<MatchedSegment> segments;
    std::vector<std::string> warnings;
    std::size_t confident_windows = 0;
    std::size_t global_searches = 0;
    bool aborted_early = false; ///< Stopped because almost no window matched

    [[nodiscard]] double confident_fraction() const noexcept {
        return windows.empty() ? 0.0
                               : static_cast<double>(confident_windows) /
                                     static_cast<double>(windows.size());
    }
};

} // namespace ttrally::alignment
