// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <span>

namespace ttrally::alignment {

/// Result of searching a short signal (needle) in a long one (haystack).
struct MatchResult {
    bool valid = false;       ///< False if the needle is silent or the search range is empty
    std::size_t lag = 0;      ///< Haystack index where the best match starts
    float peak = 0.0F;        ///< Normalised cross-correlation (Pearson) at the best lag
    float second_peak = 0.0F; ///< Highest correlation outside the exclusion zone around `lag`
    float confidence = 0.0F;  ///< peak / max(second_peak, 0.01)
};

/// Domain service interface: finds the lag in [lag_lo, lag_hi] at which the needle correlates
/// best with the haystack, using zero-mean normalised cross-correlation. The second peak ignores
/// all lags within at least `exclusion_radius` samples of the best lag.
class SignalMatcher {
  public:
    virtual ~SignalMatcher() = default;
    [[nodiscard]] virtual MatchResult find_best_match(std::span<const float> needle,
                                                      std::span<const float> haystack,
                                                      std::size_t lag_lo, std::size_t lag_hi,
                                                      std::size_t exclusion_radius) const = 0;
};

} // namespace ttrally::alignment
