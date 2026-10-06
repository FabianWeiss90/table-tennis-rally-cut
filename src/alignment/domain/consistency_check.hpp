// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/signal_alignment.hpp"

#include <string>
#include <vector>

namespace ttrally::alignment {

/// Checks that segments have a duration, lie inside the original, and appear in the original in
/// increasing, non-overlapping order. Returns one warning per violation.
[[nodiscard]] std::vector<std::string> check_consistency(
    const std::vector<MatchedSegment>& segments, std::size_t original_length, int sample_rate);

} // namespace ttrally::alignment
