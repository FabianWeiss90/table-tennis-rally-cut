// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>

namespace ttrally::alignment {

/// Parameters of the audio alignment. All durations in seconds.
struct AlignmentSettings {
    int sample_rate = 8000;              ///< Rate both audio signals are compared at
    double window_s = 2.0;               ///< Length of a window of the cut audio
    double hop_s = 0.5;                  ///< Distance between window starts
    double local_search_s = 120.0;       ///< Search range after the previous offset
    double local_back_s = 1.0;           ///< Search range before the previous offset
    double min_confidence = 2.0;         ///< Peak ratio below which a window is uncertain
    double min_peak = 0.3;               ///< Correlation below which a window is uncertain
    double exclusion_radius_s = 0.1;     ///< Zone around the main peak ignored for the 2nd peak
    double offset_tolerance_s = 0.005;   ///< Max offset difference within one segment
    double refine_half_window_s = 0.002; ///< Similarity window for boundary refinement
    float refine_threshold = 0.5F;       ///< Similarity assumed for "matches nothing"
    double max_two_sided_gap_s = 1.0;    ///< Larger unmatched gaps get two one-sided boundaries
    double min_confident_fraction = 0.1; ///< Below this, the audio is considered not matching
    unsigned threads = 0;                ///< 0 = all hardware threads

    /// Throws std::invalid_argument if a parameter is out of range.
    void validate() const;

    /// Number of samples corresponding to a duration at sample_rate (rounded).
    [[nodiscard]] std::size_t samples(double seconds) const;

    [[nodiscard]] std::size_t window_samples() const { return samples(window_s); }
    [[nodiscard]] std::size_t hop_samples() const;
};

} // namespace ttrally::alignment
