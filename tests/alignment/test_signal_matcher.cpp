// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"
#include "support/synthetic_audio.hpp"

#include <catch2/catch_test_macros.hpp>
#include <random>
#include <span>
#include <vector>

namespace {

ttrally::alignment::MatchResult find_best_match(std::span<const float> needle,
                                                std::span<const float> haystack,
                                                std::size_t lag_lo, std::size_t lag_hi,
                                                std::size_t radius, std::size_t fft_size = 65536) {
    return ttrally::alignment::PocketFftSignalMatcher(fft_size).find_best_match(
        needle, haystack, lag_lo, lag_hi, radius);
}

} // namespace

TEST_CASE("finds a noisy, scaled excerpt at the right lag") {
    const auto haystack = ttrally::test::noise_with_impulses(8000 * 60, 8000.0, 1);
    const std::size_t true_lag = 123457;
    std::vector<float> needle(haystack.begin() + true_lag, haystack.begin() + true_lag + 16000);
    std::mt19937 rng(2);
    std::normal_distribution<float> noise(0.0F, 0.02F);
    for (auto& value : needle) {
        value = 0.5F * value + noise(rng);
    }

    const auto match = find_best_match(needle, haystack, 0, haystack.size(), 800);
    REQUIRE(match.valid);
    CHECK(match.lag == true_lag);
    CHECK(match.peak > 0.9F);
    CHECK(match.confidence > 5.0F);

    SECTION("restricted search range that contains the lag") {
        const auto local = find_best_match(needle, haystack, true_lag - 5000, true_lag + 5000, 800);
        CHECK(local.lag == true_lag);
    }
    SECTION("small FFT size gives the same result") {
        const auto small = find_best_match(needle, haystack, 0, haystack.size(), 800, 1024);
        CHECK(small.lag == true_lag);
        CHECK(std::abs(small.peak - match.peak) < 1e-3F);
    }
}

TEST_CASE("unrelated needle yields low confidence") {
    const auto haystack = ttrally::test::noise_with_impulses(8000 * 60, 8000.0, 3);
    const auto needle = ttrally::test::noise_with_impulses(16000, 8000.0, 4);
    const auto match = find_best_match(needle, haystack, 0, haystack.size(), 800);
    REQUIRE(match.valid);
    CHECK(match.peak < 0.3F);
    CHECK(match.confidence < 2.0F);
}

TEST_CASE("silent needle and empty range are invalid") {
    const auto haystack = ttrally::test::noise_with_impulses(8000 * 10, 8000.0, 5);
    const std::vector<float> silence(16000, 0.0F);
    CHECK_FALSE(find_best_match(silence, haystack, 0, haystack.size(), 800).valid);
    const std::vector<float> needle(haystack.begin(), haystack.begin() + 16000);
    CHECK_FALSE(find_best_match(needle, haystack, 70000, 60000, 800).valid);
}
