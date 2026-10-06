// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The alignment domain service on synthetic audio: all boundaries of a perturbed cut must be
// recovered within 5 ms.

#include "alignment/domain/audio_aligner.hpp"
#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"
#include "support/synthetic_audio.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using ttrally::alignment::AlignmentSettings;
using ttrally::alignment::AudioAligner;
using ttrally::alignment::PocketFftSignalMatcher;
using ttrally::test::CutScenario;

namespace {

constexpr double kTolerance = 0.005;

// The gap after the third piece is longer than the local search range of the settings below,
// so the fourth piece has to be found by the global fallback search.
const CutScenario& scenario() {
    static const CutScenario data = ttrally::test::make_cut_scenario(
        {{4.0, 19.37}, {31.2, 38.05}, {47.6, 75.11}, {112.4, 115.9}, {128.25, 155.0}}, 160.0,
        3.0, 42);
    return data;
}

AlignmentSettings settings() {
    AlignmentSettings s;
    s.sample_rate = CutScenario::kRate;
    s.local_search_s = 30.0;
    return s;
}

double seconds(std::ptrdiff_t samples) {
    return static_cast<double>(samples) / CutScenario::kRate;
}

} // namespace

TEST_CASE("audio aligner recovers all boundaries within 5 ms") {
    const auto& data = scenario();
    const PocketFftSignalMatcher matcher;
    const auto result = AudioAligner(matcher).align(data.original, data.cut, settings());

    CHECK_FALSE(result.aborted_early);
    CHECK(result.global_searches > 0);
    REQUIRE(result.segments.size() == data.pieces.size());
    for (std::size_t k = 0; k < data.pieces.size(); ++k) {
        INFO("segment " << k + 1);
        const auto& piece = data.pieces[k];
        const auto& segment = result.segments[k];
        const double expected_cut_start = data.cut_start(k);
        CHECK(std::abs(seconds(static_cast<std::ptrdiff_t>(segment.cut_start)) -
                       expected_cut_start) < kTolerance);
        CHECK(std::abs(seconds(static_cast<std::ptrdiff_t>(segment.cut_end)) -
                       (expected_cut_start + piece.duration())) < kTolerance);
        CHECK(std::abs(seconds(segment.original_start()) - piece.orig_start) < kTolerance);
        CHECK(std::abs(seconds(segment.original_end()) - piece.orig_end) < kTolerance);
    }
    for (const auto& warning : result.warnings) {
        INFO(warning);
        CHECK(warning.find("before the end of segment") == std::string::npos);
    }
}

TEST_CASE("audio aligner results do not depend on the thread count") {
    const auto& data = scenario();
    const PocketFftSignalMatcher matcher;
    const AudioAligner aligner(matcher);
    auto s = settings();
    s.threads = 1;
    const auto single = aligner.align(data.original, data.cut, s);
    s.threads = 7;
    const auto multi = aligner.align(data.original, data.cut, s);

    REQUIRE(single.windows.size() == multi.windows.size());
    for (std::size_t i = 0; i < single.windows.size(); ++i) {
        CHECK(single.windows[i].offset == multi.windows[i].offset);
        CHECK(single.windows[i].confident == multi.windows[i].confident);
    }
    REQUIRE(single.segments.size() == multi.segments.size());
    for (std::size_t k = 0; k < single.segments.size(); ++k) {
        CHECK(single.segments[k].cut_start == multi.segments[k].cut_start);
        CHECK(single.segments[k].cut_end == multi.segments[k].cut_end);
        CHECK(single.segments[k].offset == multi.segments[k].offset);
    }
}

TEST_CASE("audio that is not from the original does not match") {
    const auto unrelated = ttrally::test::noise_with_impulses(30 * CutScenario::kRate,
                                                              CutScenario::kRate, 1234);
    const PocketFftSignalMatcher matcher;
    const auto result = AudioAligner(matcher).align(scenario().original, unrelated, settings());
    CHECK(result.confident_fraction() < 0.1);
}

TEST_CASE("audio aligner rejects signals shorter than one window and invalid settings") {
    const std::vector<float> original(CutScenario::kRate * 10, 0.1F);
    const std::vector<float> cut(CutScenario::kRate, 0.1F);
    const PocketFftSignalMatcher matcher;
    const AudioAligner aligner(matcher);
    CHECK_THROWS_AS(aligner.align(original, cut, settings()), std::invalid_argument);
    auto invalid = settings();
    invalid.hop_s = 0.0;
    CHECK_THROWS_AS(aligner.align(original, original, invalid), std::invalid_argument);
}
