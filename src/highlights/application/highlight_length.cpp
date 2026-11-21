// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/application/highlight_length.hpp"

#include <algorithm>
#include <limits>

namespace ttrally::highlights {

std::vector<double> clip_lengths(const std::vector<HighlightRally>& rallies, HighlightVideo video,
                                 cutting::Margins margins, double fps) {
    std::vector<cutting::TimeSpan> marked;
    if (fps <= 0) {
        return {};
    }
    for (const HighlightRally& rally : rallies) {
        if (video == HighlightVideo::MatchDay ? rally.day : rally.game) {
            marked.push_back({static_cast<double>(rally.start_frame) / fps,
                              static_cast<double>(rally.end_frame + 1) / fps});
        }
    }
    std::vector<double> lengths;
    for (const cutting::CutSection& section :
         cutting::plan_cut(std::move(marked), margins, 0.0,
                           std::numeric_limits<double>::max())) {
        lengths.push_back(section.duration_s());
    }
    return lengths;
}

double joined_length(std::span<const double> clips, double crossfade_s) {
    double total = 0.0;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        total += clips[i];
        if (i > 0) {
            total -= std::min({std::max(0.0, crossfade_s), clips[i - 1] / 2, clips[i] / 2});
        }
    }
    return total;
}

} // namespace ttrally::highlights
