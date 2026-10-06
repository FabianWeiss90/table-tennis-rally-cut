// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/alignment_settings.hpp"
#include "alignment/domain/signal_alignment.hpp"
#include "alignment/domain/signal_matcher.hpp"

#include <optional>
#include <span>

namespace ttrally::alignment {

/// Matches single windows of the cut audio in the original audio.
///
/// Segments appear in the original in increasing order, so a window is first searched locally
/// after the previous offset; only if that is not confident, the whole original is searched.
class WindowSearch {
  public:
    WindowSearch(const SignalMatcher& matcher, std::span<const float> original,
                 std::span<const float> cut, const AlignmentSettings& settings);

    /// Fills offset, peak, confidence and flags of `window` (cut_index must be set).
    void match(WindowMatch& window, std::optional<std::ptrdiff_t> previous_offset) const;

  private:
    [[nodiscard]] MatchResult search_locally(std::span<const float> needle,
                                             std::size_t cut_index,
                                             std::ptrdiff_t previous_offset) const;
    [[nodiscard]] bool is_confident(const MatchResult& match) const;

    const SignalMatcher& matcher_;
    std::span<const float> original_;
    std::span<const float> cut_;
    const AlignmentSettings& settings_;
    std::size_t window_length_;
    std::size_t max_lag_;
    std::size_t forward_;
    std::size_t back_;
    std::size_t exclusion_radius_;
};

} // namespace ttrally::alignment
