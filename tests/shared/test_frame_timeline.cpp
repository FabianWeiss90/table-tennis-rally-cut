// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/kernel/frame_timeline.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

using ttrally::FrameTimeline;
using ttrally::Rational;

TEST_CASE("constant 60 fps maps times to nearest frames") {
    const auto timeline = FrameTimeline::constant({60, 1}, 0.0);
    CHECK(timeline.is_constant());
    CHECK(timeline.time_to_frame(0.0) == 0);
    CHECK(timeline.time_to_frame(1.0) == 60);
    CHECK(timeline.time_to_frame(1.0 + 0.4 / 60.0) == 60);
    CHECK(timeline.time_to_frame(1.0 + 0.6 / 60.0) == 61);
    CHECK(timeline.frame_to_time(120) == Catch::Approx(2.0));
}

TEST_CASE("constant 59.94 fps uses the exact rate 60000/1001") {
    const auto timeline = FrameTimeline::constant({60000, 1001}, 0.0);
    // After one hour, 60 fps and 59.94 fps differ by 216 frames.
    CHECK(timeline.time_to_frame(3600.0) == 215784);
    CHECK(timeline.frame_to_time(60000) == Catch::Approx(1001.0));
    for (const std::int64_t frame : {0, 1, 1000, 123456}) {
        CHECK(timeline.time_to_frame(timeline.frame_to_time(frame)) == frame);
    }
}

TEST_CASE("constant frame rate respects the video start time") {
    const auto timeline = FrameTimeline::constant({30, 1}, 0.5);
    CHECK(timeline.time_to_frame(0.5) == 0);
    CHECK(timeline.time_to_frame(1.5) == 30);
    CHECK(timeline.frame_to_time(0) == Catch::Approx(0.5));
}

TEST_CASE("time exactly between two frames maps to the earlier one") {
    const auto timeline = FrameTimeline::constant({10, 1}, 0.0);
    CHECK(timeline.time_to_frame(0.15) == 1);
}

TEST_CASE("PTS list on a 59.94 fps grid is detected as constant frame rate") {
    // 59.94 fps in a 1/90000 time base: 1501.5 ticks per frame, stored as 1501/1502
    std::vector<std::int64_t> pts;
    for (std::int64_t i = 0; i < 1000; ++i) {
        pts.push_back(std::llround(static_cast<double>(i) * 1501.5) + 3003);
    }
    const auto timeline = FrameTimeline::from_pts(pts, {1, 90000}, {60000, 1001});
    CHECK(timeline.is_constant());
    CHECK(timeline.start_time() == Catch::Approx(3003.0 / 90000.0));
    CHECK(timeline.time_to_frame(timeline.start_time() + 10.0) == 599);
}

TEST_CASE("PTS list in decode order is sorted before analysis") {
    const std::vector<std::int64_t> pts{0, 3000, 1000, 2000, 4000}; // B-frame reordering
    const auto timeline = FrameTimeline::from_pts(pts, {1, 30000}, {30, 1});
    CHECK(timeline.is_constant());
}

TEST_CASE("PTS list with irregular spacing is treated as variable frame rate") {
    // 60 fps with a dropped frame after frame 2 and a doubled interval later
    const std::vector<std::int64_t> pts{0, 1000, 2000, 4000, 5000, 6000, 8000};
    const auto timeline = FrameTimeline::from_pts(pts, {1, 60000}, {60, 1});
    REQUIRE_FALSE(timeline.is_constant());
    CHECK(timeline.frame_count() == 7);
    CHECK(timeline.time_to_frame(0.0) == 0);
    CHECK(timeline.time_to_frame(3000.0 / 60000.0) == 2);  // tie between frames 2 and 3
    CHECK(timeline.time_to_frame(3100.0 / 60000.0) == 3);
    CHECK(timeline.time_to_frame(8000.0 / 60000.0) == 6);
    CHECK(timeline.time_to_frame(-1.0) == 0);   // clamped
    CHECK(timeline.time_to_frame(100.0) == 6);  // clamped
    CHECK(timeline.frame_to_time(3) == Catch::Approx(4000.0 / 60000.0));
}

TEST_CASE("PTS list drifting away from the nominal rate is treated as variable") {
    // Nominal 60 fps, actual 59.94 fps: drifts by more than 0.1 frame after ~100 frames
    std::vector<std::int64_t> pts;
    for (std::int64_t i = 0; i < 1000; ++i) {
        pts.push_back(i * 1001);
    }
    const auto timeline = FrameTimeline::from_pts(pts, {1, 60000}, {60, 1});
    CHECK_FALSE(timeline.is_constant());
    CHECK(timeline.time_to_frame(500.0 * 1001.0 / 60000.0) == 500);
}
