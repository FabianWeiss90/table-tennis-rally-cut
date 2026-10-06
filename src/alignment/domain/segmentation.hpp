// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/signal_alignment.hpp"

#include <vector>

namespace ttrally::alignment {

/// Groups consecutive confident windows whose offsets differ by at most `offset_tolerance`
/// samples into segments. Offset and confidence of a segment are the medians of its windows.
/// The segment boundaries (cut_start/cut_end) are left for boundary refinement.
[[nodiscard]] std::vector<MatchedSegment> group_into_segments(
    const std::vector<WindowMatch>& windows, std::size_t offset_tolerance);

} // namespace ttrally::alignment
