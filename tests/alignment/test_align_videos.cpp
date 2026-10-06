// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The AlignVideos use case with in-memory fakes for all media ports.

#include "alignment/application/align_videos.hpp"
#include "alignment/domain/alignment_error.hpp"
#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"
#include "support/fake_media.hpp"
#include "support/synthetic_audio.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>

using namespace ttrally;
using namespace ttrally::alignment;
using test::CutScenario;

namespace {

constexpr int kFps = 60;
const std::filesystem::path kOriginal = "original.mp4";
const std::filesystem::path kCut = "cut.mp4";

const CutScenario& scenario() {
    static const CutScenario data = test::make_cut_scenario(
        {{3.0, 12.5}, {20.25, 26.0}, {40.0, 51.3}}, 60.0, 0.0, 11);
    return data;
}

media::MediaInfo video_info(bool with_audio) {
    media::MediaInfo info;
    info.container = "fake";
    info.duration_s = 60.0;
    media::VideoStreamInfo video;
    video.index = 0;
    video.codec = "fake";
    video.time_base = {1, kFps};
    video.avg_frame_rate = {kFps, 1};
    info.video = video;
    if (with_audio) {
        media::AudioStreamInfo audio;
        audio.index = 1;
        audio.codec = "fake";
        audio.sample_rate = CutScenario::kRate;
        audio.channels = 1;
        info.audio = audio;
    }
    return info;
}

media::AudioSignal signal(std::vector<float> samples) {
    return {std::move(samples), CutScenario::kRate, 0.0};
}

media::VideoTimestamps frames(int count) {
    media::VideoTimestamps timestamps;
    timestamps.time_base = {1, kFps};
    for (int i = 0; i < count; ++i) {
        timestamps.pts.push_back(i);
    }
    return timestamps;
}

/// A non-constant test image that depends only on `seed`.
media::GrayImage pattern(int seed) {
    media::GrayImage image{64, 36, std::vector<std::uint8_t>(64 * 36)};
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        const std::size_t value = i * 37 + static_cast<std::size_t>(seed) * 101;
        image.pixels[i] = static_cast<std::uint8_t>(value % 251);
    }
    return image;
}

struct Fixture {
    test::FakeMediaLibrary library;
    PocketFftSignalMatcher matcher;
    SilentProgressReporter progress;
    bool frames_match = true;
    test::FakeFrameSamplerFactory samplers{[this](const std::filesystem::path& path, double) {
        return pattern(frames_match || path == kOriginal ? 1 : 2);
    }};

    Fixture() {
        library.add(kOriginal, {video_info(true), signal(scenario().original), frames(60 * kFps)});
        library.add(kCut, {video_info(true), signal(scenario().cut), frames(30 * kFps)});
    }

    AlignmentReport run(AlignVideosRequest request = default_request()) {
        AlignVideos use_case(library, library, samplers, matcher, progress);
        return use_case.execute(request);
    }

    static AlignVideosRequest default_request() {
        AlignVideosRequest request;
        request.original = kOriginal;
        request.cut = kCut;
        request.settings.local_search_s = 10.0;
        return request;
    }
};

} // namespace

TEST_CASE("AlignVideos maps every piece of the cut to times and frames of the original") {
    Fixture fixture;
    const auto report = fixture.run();

    const auto& pieces = scenario().pieces;
    REQUIRE(report.candidates.size() == pieces.size());
    for (std::size_t k = 0; k < pieces.size(); ++k) {
        INFO("segment " << k + 1);
        const auto& candidate = report.candidates[k];
        CHECK(std::abs(candidate.orig_start_s - pieces[k].orig_start) < 0.005);
        CHECK(std::abs(candidate.orig_end_s - pieces[k].orig_end) < 0.005);
        CHECK(std::llabs(candidate.orig_start_frame - std::llround(pieces[k].orig_start * kFps)) <=
              1);
        REQUIRE(candidate.visual_similarity);
        CHECK(*candidate.visual_similarity > 0.99);
    }
    CHECK(report.gaps.size() == pieces.size() + 1); // before, between and after
    CHECK(report.original_constant_frame_rate);
    CHECK(report.original_frame_count == 60 * kFps);
    CHECK(report.warnings.empty());
}

TEST_CASE("AlignVideos reports segments whose frames do not match") {
    Fixture fixture;
    fixture.frames_match = false;
    const auto report = fixture.run();
    REQUIRE(report.visual_check);
    CHECK(report.warnings.size() == scenario().pieces.size());
    CHECK(report.warnings.front().find("frames from the middle of the segment differ") !=
          std::string::npos);
}

TEST_CASE("AlignVideos skips the visual check on request") {
    Fixture fixture;
    auto request = Fixture::default_request();
    request.visual_check = false;
    const auto report = fixture.run(request);
    CHECK_FALSE(report.visual_check);
    CHECK_FALSE(report.candidates.front().visual_similarity);
}

TEST_CASE("AlignVideos rejects files without audio or with unrelated audio") {
    Fixture fixture;
    SECTION("cut without audio") {
        fixture.library.add(kCut, {video_info(false), signal({}), frames(10)});
        CHECK_THROWS_AS(fixture.run(), AlignmentNotPossible);
        CHECK(fixture.library.reads == 0); // checked before decoding
    }
    SECTION("audio not from the original") {
        fixture.library.add(kCut, {video_info(true),
                                   signal(test::noise_with_impulses(30 * CutScenario::kRate,
                                                                    CutScenario::kRate, 77)),
                                   frames(10)});
        CHECK_THROWS_AS(fixture.run(), AlignmentNotPossible);
    }
}
