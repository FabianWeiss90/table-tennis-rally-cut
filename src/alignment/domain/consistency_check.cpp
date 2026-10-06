// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/consistency_check.hpp"

#include <format>

namespace ttrally::alignment {

std::vector<std::string> check_consistency(const std::vector<MatchedSegment>& segments,
                                           std::size_t original_length, int sample_rate) {
    auto seconds = [sample_rate](std::ptrdiff_t samples) {
        return static_cast<double>(samples) / sample_rate;
    };
    std::vector<std::string> warnings;
    for (std::size_t k = 0; k < segments.size(); ++k) {
        const MatchedSegment& segment = segments[k];
        const std::size_t number = k + 1;
        if (segment.cut_end <= segment.cut_start) {
            warnings.push_back(std::format(
                "Segment {} has no duration after boundary refinement; check it manually.",
                number));
        }
        if (segment.window_count == 1) {
            warnings.push_back(std::format(
                "Segment {} is supported by a single window only; check it manually.", number));
        }
        if (segment.original_start() < 0 ||
            segment.original_end() > static_cast<std::ptrdiff_t>(original_length)) {
            warnings.push_back(std::format("Segment {} extends beyond the original ({:.3f} s to "
                                           "{:.3f} s).",
                                           number, seconds(segment.original_start()),
                                           seconds(segment.original_end())));
        }
        if (k > 0 && segment.original_start() < segments[k - 1].original_end()) {
            warnings.push_back(std::format(
                "Segment {} starts {:.3f} s before the end of segment {} in the original "
                "(overlap or non-increasing order).",
                number, seconds(segments[k - 1].original_end() - segment.original_start()), k));
        }
    }
    return warnings;
}

} // namespace ttrally::alignment
