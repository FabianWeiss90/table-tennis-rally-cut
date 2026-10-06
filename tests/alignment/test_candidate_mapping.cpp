// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/candidate_mapping.hpp"
#include "alignment/domain/consistency_check.hpp"
#include "alignment/domain/segmentation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ttrally::alignment;
using ttrally::FrameTimeline;
using ttrally::media::AudioSignal;

namespace {

constexpr int kRate = 8000;

MatchedSegment segment(double cut_start, double cut_end, double offset) {
    MatchedSegment s;
    s.cut_start = static_cast<std::size_t>(cut_start * kRate);
    s.cut_end = static_cast<std::size_t>(cut_end * kRate);
    s.offset = static_cast<std::ptrdiff_t>(offset * kRate);
    s.window_count = 5;
    return s;
}

AudioSignal silent_signal(double start_time) {
    AudioSignal signal;
    signal.sample_rate = kRate;
    signal.start_time_s = start_time;
    return signal;
}

} // namespace

TEST_CASE("segments are mapped to times and frames of the original") {
    const FrameTimeline timeline = FrameTimeline::constant({60, 1}, 0.0);
    const OriginalFrames frames{timeline, 60 * 100}; // 100 s of video
    const std::vector<MatchedSegment> segments{segment(0.0, 10.0, 5.0), segment(10.0, 15.0, 40.0)};

    const auto candidates =
        map_to_candidates(segments, silent_signal(0.0), silent_signal(0.0), frames);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0].id == 1);
    CHECK(candidates[0].orig_start_s == Catch::Approx(5.0));
    CHECK(candidates[0].orig_end_s == Catch::Approx(15.0));
    CHECK(candidates[0].orig_start_frame == 300);
    CHECK(candidates[0].orig_end_frame == 899); // end time is exclusive
    CHECK(candidates[1].orig_start_s == Catch::Approx(50.0));
    CHECK(candidates[1].offset_s == Catch::Approx(40.0));

    const auto gaps = find_gaps(candidates, frames);
    REQUIRE(gaps.size() == 3);
    CHECK(gaps[0].after_candidate == 0);
    CHECK(gaps[0].orig_start_frame == 0);
    CHECK(gaps[0].orig_end_frame == 299);
    CHECK(gaps[1].after_candidate == 1);
    CHECK(gaps[1].orig_start_frame == 900);
    CHECK(gaps[1].orig_end_frame == 2999);
    CHECK(gaps[1].duration_s() == Catch::Approx(35.0));
    CHECK(gaps[2].after_candidate == 2);
    CHECK(gaps[2].orig_end_frame == 5999);
}

TEST_CASE("audio start times shift the mapped times") {
    const FrameTimeline timeline = FrameTimeline::constant({60, 1}, 0.0);
    const OriginalFrames frames{timeline, 6000};
    const auto candidates = map_to_candidates({segment(0.0, 10.0, 5.0)}, silent_signal(0.5),
                                              silent_signal(0.1), frames);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].cut_start_s == Catch::Approx(0.1));
    CHECK(candidates[0].orig_start_s == Catch::Approx(5.5));
    CHECK(candidates[0].offset_s == Catch::Approx(5.4));
}

TEST_CASE("consecutive windows with equal offsets form one segment") {
    std::vector<WindowMatch> windows(6);
    const std::ptrdiff_t offsets[] = {100, 102, 100, 900, 901, 900};
    for (std::size_t i = 0; i < windows.size(); ++i) {
        windows[i].cut_index = i * 10;
        windows[i].offset = offsets[i];
        windows[i].confidence = 5.0F;
        windows[i].confident = true;
    }
    windows[1].confident = false; // uncertain windows are skipped
    const auto segments = group_into_segments(windows, 40);
    REQUIRE(segments.size() == 2);
    CHECK(segments[0].first_window == 0);
    CHECK(segments[0].last_window == 2);
    CHECK(segments[0].window_count == 2);
    CHECK(segments[1].offset == 900);
}

TEST_CASE("overlapping segments in the original are reported") {
    auto a = segment(0.0, 10.0, 20.0);  // original 20..30
    auto b = segment(10.0, 20.0, 15.0); // original 25..35 overlaps
    const auto warnings = check_consistency({a, b}, 100 * kRate, kRate);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("Segment 2 starts 5.000 s before the end of segment 1") !=
          std::string::npos);
}
