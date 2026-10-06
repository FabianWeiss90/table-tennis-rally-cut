// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/alignment_settings.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ttrally::alignment {

void AlignmentSettings::validate() const {
    const bool valid = sample_rate > 0 && window_s > 0.0 && hop_s > 0.0 && local_search_s > 0.0 &&
                       local_back_s >= 0.0 && min_confidence > 0.0 && exclusion_radius_s > 0.0 &&
                       offset_tolerance_s >= 0.0 && refine_half_window_s > 0.0 &&
                       max_two_sided_gap_s >= 0.0 && min_confident_fraction >= 0.0 &&
                       min_confident_fraction <= 1.0;
    if (!valid) {
        throw std::invalid_argument("invalid alignment settings");
    }
}

std::size_t AlignmentSettings::samples(double seconds) const {
    return static_cast<std::size_t>(std::llround(seconds * sample_rate));
}

std::size_t AlignmentSettings::hop_samples() const {
    return std::max<std::size_t>(1, samples(hop_s));
}

} // namespace ttrally::alignment
