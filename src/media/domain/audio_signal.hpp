// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <vector>

namespace ttrally::media {

/// Mono audio at a fixed sample rate, positioned on the timeline of the file it came from.
struct AudioSignal {
    std::vector<float> samples;
    int sample_rate = 0;
    double start_time_s = 0.0; ///< Presentation time of samples[0]

    /// Presentation time of a (possibly fractional or out-of-range) sample position.
    [[nodiscard]] double time_at(double sample_position) const noexcept {
        return start_time_s + sample_position / sample_rate;
    }

    [[nodiscard]] double duration_s() const noexcept {
        return static_cast<double>(samples.size()) / sample_rate;
    }
};

} // namespace ttrally::media
