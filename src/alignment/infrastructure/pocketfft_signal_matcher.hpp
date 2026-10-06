// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/signal_matcher.hpp"

namespace ttrally::alignment {

/// SignalMatcher computing the correlation blockwise with FFTs (overlap-save, pocketfft), so
/// memory use is independent of the search range. Thread-safe.
class PocketFftSignalMatcher final : public SignalMatcher {
  public:
    static constexpr std::size_t kDefaultFftSize = std::size_t{1} << 16U;

    explicit PocketFftSignalMatcher(std::size_t fft_size = kDefaultFftSize)
        : fft_size_(fft_size) {}

    [[nodiscard]] MatchResult find_best_match(std::span<const float> needle,
                                              std::span<const float> haystack, std::size_t lag_lo,
                                              std::size_t lag_hi,
                                              std::size_t exclusion_radius) const override;

  private:
    std::size_t fft_size_;
};

} // namespace ttrally::alignment
