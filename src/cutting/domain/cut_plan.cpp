// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/domain/cut_plan.hpp"

#include <algorithm>
#include <stdexcept>

namespace ttrally::cutting {

std::vector<CutSection> plan_cut(std::vector<TimeSpan> rallies, Margins margins,
                                 double video_start_s, double video_end_s) {
    if (margins.before_s < 0 || margins.after_s < 0) {
        throw std::invalid_argument("margins must not be negative");
    }
    std::ranges::sort(rallies, {}, &TimeSpan::start_s);
    std::vector<CutSection> sections;
    for (const TimeSpan& rally : rallies) {
        const double start = std::max(video_start_s, rally.start_s - margins.before_s);
        const double end = std::min(video_end_s, rally.end_s + margins.after_s);
        if (end <= start) {
            continue; // outside the video
        }
        if (!sections.empty() && start <= sections.back().end_s) {
            sections.back().end_s = std::max(sections.back().end_s, end);
            ++sections.back().rallies;
        } else {
            sections.push_back({start, end, 1});
        }
    }
    return sections;
}

double total_duration_s(const std::vector<CutSection>& sections) noexcept {
    double total = 0.0;
    for (const CutSection& section : sections) {
        total += section.duration_s();
    }
    return total;
}

} // namespace ttrally::cutting
