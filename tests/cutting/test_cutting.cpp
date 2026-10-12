// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Cut planning and the CutVideo use case with in-memory fakes.

#include "cutting/application/cut_video.hpp"
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
        file.timestamps.time_base = {1, kFps};
        for (int i = 0; i < kFrames; ++i) {
            file.timestamps.pts.push_back(i);
        }
        library.add("match.mp4", file);
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
