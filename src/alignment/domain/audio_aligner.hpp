// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/alignment_settings.hpp"
#include "alignment/domain/signal_alignment.hpp"
#include "alignment/domain/signal_matcher.hpp"

#include <span>

namespace ttrally::alignment {

/// Domain service: finds which pieces of a cut audio signal come from where in the original.
///
/// 1. Split the cut into overlapping windows and match each in the original (WindowSearch).
/// 2. Group windows with equal offsets into segments.
/// 3. Remove spurious segments that would break the increasing order in the original.
/// 4. Locate the segment boundaries with sample accuracy.
/// 5. Check the segments for consistency.
///
/// Results are deterministic regardless of the number of threads.
class AudioAligner {
  public:
    explicit AudioAligner(const SignalMatcher& matcher) : matcher_(matcher) {}

    /// Both signals must be sampled at settings.sample_rate. Throws std::invalid_argument for
    /// invalid settings or signals shorter than one window.
    [[nodiscard]] SignalAlignment align(std::span<const float> original,
                                        std::span<const float> cut,
                                        const AlignmentSettings& settings) const;

  private:
    /// Matches all windows; stops early if almost nothing matches. Returns false if stopped.
    bool match_windows(SignalAlignment& result, std::span<const float> original,
                       std::span<const float> cut, const AlignmentSettings& settings) const;

    const SignalMatcher& matcher_;
};

} // namespace ttrally::alignment
