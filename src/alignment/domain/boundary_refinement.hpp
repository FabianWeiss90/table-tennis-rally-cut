// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/alignment_settings.hpp"
#include "alignment/domain/signal_alignment.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ttrally::alignment {

/// Locates the cut point between two pieces of the cut signal.
///
/// Before the boundary the cut signal is expected to match original[t + *before_offset], after
/// it original[t + *after_offset]. A missing offset means "matches nothing" (start or end of the
/// cut, or inserted material); that side is scored with `threshold`.
///
/// For every sample in [lo, hi) a gain-invariant similarity to both predictions is computed over
/// +-half_window samples, so fades do not bias the result. The boundary is the position that
/// maximises the advantage of the "before" prediction on its left and of the "after" prediction
/// on its right. Returns a cut sample index in [lo, hi].
[[nodiscard]] std::size_t locate_boundary(std::span<const float> cut,
                                          std::span<const float> original, std::size_t lo,
                                          std::size_t hi,
                                          std::optional<std::ptrdiff_t> before_offset,
                                          std::optional<std::ptrdiff_t> after_offset,
                                          std::size_t half_window, float threshold);

/// Sets cut_start/cut_end of all segments by locating each boundary near the confident windows.
/// Neighbouring segments share a boundary unless an unmatched gap lies between them.
void refine_segment_boundaries(std::vector<MatchedSegment>& segments,
                               const std::vector<WindowMatch>& windows,
                               std::span<const float> original, std::span<const float> cut,
                               const AlignmentSettings& settings);

} // namespace ttrally::alignment
