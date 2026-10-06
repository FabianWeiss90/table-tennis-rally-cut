// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/signal_alignment.hpp"

#include <string>
#include <vector>

namespace ttrally::alignment {

/// Groups consecutive confident windows whose offsets differ by at most `offset_tolerance`
/// samples into segments. Offset and confidence of a segment are the medians of its windows.
/// The segment boundaries (cut_start/cut_end) are left for boundary refinement.
[[nodiscard]] std::vector<MatchedSegment> group_into_segments(
    const std::vector<WindowMatch>& windows, std::size_t offset_tolerance);

/// Enforces that segments appear in the original in increasing order, i.e. that the offset never
/// decreases from one segment to the next (by more than `offset_tolerance` samples). Of two
/// conflicting neighbours the weaker one (fewer windows, then lower confidence) is a spurious
/// match, typically of a window straddling a cut, and is removed. Returns one warning per
/// removed segment, with times in seconds from the start of the cut.
[[nodiscard]] std::vector<std::string> remove_out_of_order_segments(
    std::vector<MatchedSegment>& segments, const std::vector<WindowMatch>& windows,
    std::size_t offset_tolerance, int sample_rate);

} // namespace ttrally::alignment
