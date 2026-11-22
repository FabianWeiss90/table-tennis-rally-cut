// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Cut planning and the CutVideo use case with in-memory fakes.

#include "cutting/application/cut_video.hpp"
#include "cutting/application/merge_videos.hpp"
#include "cutting/domain/resolution.hpp"
#include "support/fake_media.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace ttrally;
using namespace ttrally::cutting;
using Catch::Approx;

namespace {

constexpr int kFps = 60;
constexpr int kFrames = 1200; // 20 s

class MemoryCutList final : public annotation::AnnotationRepository {
  public:
    annotation::StoredLabels load(const std::string& video_id) override {
        requested = video_id;
        return labels;
    }
    void save(const annotation::AnnotationSheet&) override {}
    annotation::StoredLabels labels;
    std::string requested;
};

class RecordingCutter final : public VideoCutter {
  public:
    CutResult cut(const std::filesystem::path&, std::span<const CutSection> sections,
                  const std::filesystem::path& output, const CutOptions& options,
                  ProgressReporter&) override {
        cut_sections.assign(sections.begin(), sections.end());
        written = output;
        mode = options.mode;
        return {"fake", total_duration_s(cut_sections)};
    }
    std::vector<CutSection> cut_sections;
    std::filesystem::path written;
    CutMode mode = CutMode::Exact;
};

struct Fixture {
    test::FakeMediaLibrary library;
    MemoryCutList cut_list;
    RecordingCutter cutter;
    SilentProgressReporter progress;

    Fixture() {
        test::FakeFile file;
        file.info.video = media::VideoStreamInfo{};
        file.info.video->index = 0;
        file.info.video->avg_frame_rate = {kFps, 1};
        file.info.video->width = 3840;
        file.info.video->height = 2160;
        file.timestamps.time_base = {1, kFps};
        for (int i = 0; i < kFrames; ++i) {
            file.timestamps.pts.push_back(i);
        }
        library.add("match.mp4", file);
    }

    CutVideoResult run_named(CutOptions options) {
        cut_list.labels.rallies = {{100, 200, {}, false, ""}};
        CutVideo cut(library, library, cut_list, cutter, progress);
        return cut.execute({.video = "match.mp4",
                            .video_id = "match",
                            .output = "cuts/match.mp4",
                            .margins = {},
                            .options = options,
                            .resolution_in_name = true});
    }

    CutVideoResult run(Margins margins = {}) {
        CutVideo cut(library, library, cut_list, cutter, progress);
        return cut.execute({.video = "match.mp4",
                            .video_id = "match",
                            .output = "out.mp4",
                            .margins = margins,
                            .options = {}});
    }
};

} // namespace

TEST_CASE("the written height is added to the file name") {
    CHECK(output_height(2160, 0) == 2160);
    CHECK(output_height(2160, 1080) == 1080);
    CHECK(output_height(1080, 2160) == 1080); // never enlarged
    CHECK(output_height(1081, 0) == 1080);    // even
    CHECK(with_resolution("cuts/match.mp4", 1080) ==
          std::filesystem::path("cuts") / "match-1080p.mp4");
    Fixture fixture;
    CHECK(fixture.run_named({.max_height = 1080}).output ==
          std::filesystem::path("cuts") / "match-1080p.mp4");
    CHECK(fixture.cutter.written == std::filesystem::path("cuts") / "match-1080p.mp4");
    CHECK(fixture.run_named({}).output == std::filesystem::path("cuts") / "match-2160p.mp4");
    // A fast cut copies the video: always the original's height
    CHECK(fixture.run_named({.mode = CutMode::Fast, .max_height = 1080}).output ==
          std::filesystem::path("cuts") / "match-2160p.mp4");
    CHECK(fixture.run().output == "out.mp4"); // a given name stays as it is
}

TEST_CASE("rallies get their margins and stay inside the video") {
    const auto sections =
        plan_cut({{10.0, 15.0}, {1.0, 3.0}}, {.before_s = 2.0, .after_s = 1.5}, 0.0, 16.0);
    REQUIRE(sections.size() == 2);
    CHECK(sections[0].start_s == 0.0); // limited by the start of the video
    CHECK(sections[0].end_s == 4.5);
    CHECK(sections[1].start_s == 8.0);
    CHECK(sections[1].end_s == 16.0); // limited by the end of the video
    CHECK(total_duration_s(sections) == Approx(12.5));
}

TEST_CASE("rallies whose margins overlap or touch become one section") {
    const auto sections = plan_cut({{10.0, 12.0}, {15.0, 17.0}, {21.0, 22.0}, {40.0, 41.0}},
                                   {.before_s = 2.0, .after_s = 2.0}, 0.0, 100.0);
    REQUIRE(sections.size() == 2);
    CHECK(sections[0].start_s == 8.0);
    CHECK(sections[0].end_s == 24.0); // 14-19 overlaps, 19 touches 19
    CHECK(sections[0].rallies == 3);
    CHECK(sections[1].rallies == 1);
    CHECK_THROWS_AS(plan_cut({}, {.before_s = -1.0, .after_s = 0.0}, 0.0, 1.0),
                    std::invalid_argument);
    CHECK(plan_cut({}, {}, 0.0, 1.0).empty());
}

TEST_CASE("the cut list's frames become sections on frame boundaries") {
    Fixture fixture;
    fixture.cut_list.labels.rallies = {{120, 179, {}, false, ""}, {600, 659, {}, false, ""}};
    fixture.cut_list.labels.ignored = {{900, 1000, ""}}; // not a rally: not cut
    const auto result = fixture.run({.before_s = 0.51, .after_s = 0.0});

    CHECK(fixture.cut_list.requested == "match");
    CHECK(result.rallies == 2);
    REQUIRE(fixture.cutter.cut_sections.size() == 2);
    // Rally 1: frames 120-179 = 2.0 s to 3.0 s; 0.51 s earlier is 1.49 s, the next frame 1.5 s
    CHECK(fixture.cutter.cut_sections[0].start_s == Approx(1.5));
    CHECK(fixture.cutter.cut_sections[0].end_s == Approx(3.0));
    CHECK(fixture.cutter.cut_sections[1].start_s == Approx(9.5));
    CHECK(fixture.cutter.cut_sections[1].end_s == Approx(11.0));
    CHECK(fixture.cutter.written == "out.mp4");
}

TEST_CASE("a cut list without rallies or with frames beyond the video is refused") {
    Fixture fixture;
    CHECK_THROWS_AS(fixture.run(), std::invalid_argument);
    fixture.cut_list.labels.rallies = {{1100, 1300, {}, false, ""}};
    CHECK_THROWS_AS(fixture.run(), std::invalid_argument);
    CHECK(fixture.cutter.cut_sections.empty());
}

namespace {

/// Remembers what it was asked to join.
class RecordingJoiner final : public ttrally::cutting::VideoJoiner {
  public:
    double join(std::span<const std::filesystem::path> parts, const std::filesystem::path& output,
                ttrally::ProgressReporter& /*progress*/) override {
        joined.assign(parts.begin(), parts.end());
        written = output;
        return 12.5;
    }
    std::vector<std::filesystem::path> joined;
    std::filesystem::path written;
};

} // namespace

TEST_CASE("merging appends the parts in their order into a new video") {
    RecordingJoiner joiner;
    ttrally::SilentProgressReporter progress;
    ttrally::cutting::MergeVideos merge(joiner, progress);
    const auto result = merge.execute({.parts = {"day/GX020001.MP4", "day/GX010001.MP4"},
                                       .output = "day/game.mp4"});
    CHECK(result.parts == 2);
    CHECK(result.duration_s == 12.5);
    CHECK(joiner.joined ==
          std::vector<std::filesystem::path>{"day/GX020001.MP4", "day/GX010001.MP4"});
    CHECK(joiner.written == "day/game.mp4");
}

TEST_CASE("merging refuses a single part, a part twice and replacing a part") {
    RecordingJoiner joiner;
    ttrally::SilentProgressReporter progress;
    ttrally::cutting::MergeVideos merge(joiner, progress);
    CHECK_THROWS_AS(merge.execute({.parts = {"a.mp4"}, .output = "b.mp4"}),
                    std::invalid_argument);
    CHECK_THROWS_AS(merge.execute({.parts = {"a.mp4", "./a.mp4"}, .output = "b.mp4"}),
                    std::invalid_argument);
    CHECK_THROWS_AS(merge.execute({.parts = {"a.mp4", "b.mp4"}, .output = "dir/../b.mp4"}),
                    std::invalid_argument);
    CHECK(joiner.joined.empty());
}
