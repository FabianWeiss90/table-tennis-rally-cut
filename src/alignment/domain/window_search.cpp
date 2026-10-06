// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/window_search.hpp"

#include <algorithm>

namespace ttrally::alignment {

WindowSearch::WindowSearch(const SignalMatcher& matcher, std::span<const float> original,
                           std::span<const float> cut, const AlignmentSettings& settings)
    : matcher_(matcher), original_(original), cut_(cut), settings_(settings),
      window_length_(settings.window_samples()), max_lag_(original.size() - window_length_),
      forward_(settings.samples(settings.local_search_s)),
      back_(settings.samples(settings.local_back_s)),
      exclusion_radius_(settings.samples(settings.exclusion_radius_s)) {}

void WindowSearch::match(WindowMatch& window,
                         std::optional<std::ptrdiff_t> previous_offset) const {
    const auto needle = cut_.subspan(window.cut_index, window_length_);
    MatchResult match;
    if (previous_offset) {
        match = search_locally(needle, window.cut_index, *previous_offset);
    }
    if (!is_confident(match)) {
        window.global_search = true;
        const MatchResult global =
            matcher_.find_best_match(needle, original_, 0, max_lag_, exclusion_radius_);
        const bool better = global.valid && (!match.valid || is_confident(global) ||
                                             global.confidence > match.confidence);
        if (better) {
            match = global;
        }
    }
    window.valid = match.valid;
    window.confident = is_confident(match);
    window.peak = match.peak;
    window.confidence = match.confidence;
    window.offset =
        static_cast<std::ptrdiff_t>(match.lag) - static_cast<std::ptrdiff_t>(window.cut_index);
}

MatchResult WindowSearch::search_locally(std::span<const float> needle, std::size_t cut_index,
                                         std::ptrdiff_t previous_offset) const {
    const std::ptrdiff_t expected = static_cast<std::ptrdiff_t>(cut_index) + previous_offset;
    const std::ptrdiff_t lo = expected - static_cast<std::ptrdiff_t>(back_);
    const std::ptrdiff_t hi = expected + static_cast<std::ptrdiff_t>(forward_);
    if (hi < 0 || lo > static_cast<std::ptrdiff_t>(max_lag_)) {
        return {};
    }
    return matcher_.find_best_match(needle, original_,
                                    static_cast<std::size_t>(std::max<std::ptrdiff_t>(lo, 0)),
                                    static_cast<std::size_t>(hi), exclusion_radius_);
}

bool WindowSearch::is_confident(const MatchResult& match) const {
    return match.valid && match.confidence >= settings_.min_confidence &&
           match.peak >= settings_.min_peak;
}

} // namespace ttrally::alignment
