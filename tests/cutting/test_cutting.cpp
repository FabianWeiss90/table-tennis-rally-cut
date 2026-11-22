// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Cut planning and the CutVideo use case with in-memory fakes.

#include "cutting/application/cut_video.hpp"
#include "cutting/domain/resolution.hpp"
#include "cutting/domain/white_balance.hpp"
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

/// The YUV values (limited range, BT.709) of a colour, by inverting rgb_of numerically.
std::array<double, 3> yuv_of(RgbColor color) {
    const double kr = 0.2126;
    const double kb = 0.0722;
    const double y = kr * color.red + (1 - kr - kb) * color.green + kb * color.blue;
    const double u = (color.blue - y) / (2 * (1 - kb));
    const double v = (color.red - y) / (2 * (1 - kr));
    return {16 + y * 219 / 255, 128 + u * 224 / 255, 128 + v * 224 / 255};
}

} // namespace

TEST_CASE("white balance: a reference tuned by temperature and tint, the others like it") {
    const RgbColor white = rgb_of(235, 128, 128);
    CHECK(white.red == Approx(255.0));
    CHECK(white.blue == Approx(255.0));
    const RgbColor warm{200, 180, 150};
    // The sliders that make the area neutral
    const auto [temperature, tint] = neutral_temperature_tint(warm);
    CHECK(temperature < 0); // cooler
    const RgbColor fixed = corrected(warm, temperature_tint_gains(temperature, tint, 0, warm));
    CHECK(fixed.red == Approx(fixed.green));
    CHECK(fixed.blue == Approx(fixed.green));
    CHECK(luma_of(fixed) == Approx(luma_of(warm)));
    CHECK(temperature_tint_gains(0, 0, 0, warm).identity());
    const RgbGains warmer = temperature_tint_gains(0.5, 0, 0, warm);
    CHECK(warmer.red > warmer.green);
    CHECK(warmer.blue < warmer.green);
    const RgbGains magenta = temperature_tint_gains(0, 0.5, 0, warm);
    CHECK(magenta.green < magenta.red);
    // Brightness scales all channels: 1 makes it 1.5 times as bright
    const RgbGains brighter = temperature_tint_gains(0, 0, 1, warm);
    CHECK(luma_of(corrected(warm, brighter)) == Approx(1.5 * luma_of(warm)));
    CHECK(brighter.red == Approx(brighter.blue));

    // The others look like the tuned reference: in tint by the area, in brightness by the
    // whole picture
    const RgbColor cool{150, 170, 200};
    const RgbColor dark{60, 50, 40};
    const RgbColor bright_picture{120, 120, 120}; // the reference's whole picture
    const RgbColor dark_picture{40, 40, 40};      // a darker video
    const std::vector<MeasuredFile> files{{warm, bright_picture},
                                          {cool, bright_picture},
                                          {std::nullopt, dark_picture},
                                          {dark, dark_picture}};
    WhiteBalanceSettings settings{.files = {}, .reference = "", .temperature = temperature,
                                  .tint = tint, .brightness = 0.0, .match_brightness = false};
    const auto gains = balance_gains(files, 0, settings);
    REQUIRE(gains.size() == 4);
    const RgbColor target = corrected(warm, gains[0]);
    const RgbColor other = corrected(cool, gains[1]);
    CHECK(other.red / other.green == Approx(target.red / target.green));
    CHECK(other.blue / other.green == Approx(target.blue / target.green));
    CHECK(luma_of(other) == Approx(luma_of(cool))); // its own brightness
    CHECK(gains[2].identity());                     // no area
    settings.match_brightness = true;
    const auto matched = balance_gains(files, 0, settings);
    const RgbColor lifted = corrected(dark, matched[3]);
    CHECK(lifted.red / lifted.green == Approx(target.red / target.green)); // the tint
    // The whole picture as bright as the reference's tuned whole picture, not the area
    CHECK(luma_of(corrected(dark_picture, matched[3])) ==
          Approx(luma_of(corrected(bright_picture, matched[0]))));
    CHECK(luma_of(lifted) != Approx(luma_of(target)));
    // Without the reference's area colour nothing is corrected
    for (const RgbGains& none :
         balance_gains({{std::nullopt, bright_picture}, {cool, bright_picture}}, 0, settings)) {
        CHECK(none.identity());
    }
}

TEST_CASE("the first file sets the area; the others only move it, else take the one before") {
    const RelativeRect first{0.1, 0.1, 0.3, 0.2};
    const RelativeRect moved{0.5, 0.6, 0.9, 0.99}; // only its position counts
    const RelativeRect edge{0.95, 0.95, 1.0, 1.0}; // moved beyond the picture: kept inside
    const auto areas = effective_areas({{"a", first, {}},
                                        {"b", std::nullopt, {}},
                                        {"c", moved, {}},
                                        {"d", std::nullopt, {}},
                                        {"e", edge, {}}});
    REQUIRE(areas.size() == 5);
    CHECK(areas[0] == first);
    CHECK(areas[1] == first);
    REQUIRE(areas[2]);
    CHECK(areas[2]->left == Approx(0.5));
    CHECK(areas[2]->top == Approx(0.6));
    CHECK(areas[2]->right == Approx(0.7)); // the first's size
    CHECK(areas[2]->bottom == Approx(0.7));
    CHECK(areas[3] == areas[2]);
    REQUIRE(areas[4]);
    CHECK(areas[4]->right == Approx(1.0));
    CHECK(areas[4]->left == Approx(0.8));
    // Without an area in the first file there is none
    const auto none = effective_areas({{"a", std::nullopt, {}}, {"b", moved, {}}});
    CHECK_FALSE(none[0]);
    CHECK_FALSE(none[1]);
}

TEST_CASE("the white balance applies to YUV values as a matrix") {
    const RgbGains gains{1.1, 0.95, 0.9};
    const RgbColor color{120, 140, 160};
    const auto matrix = yuv_matrix(gains);
    const auto before = yuv_of(color);
    const auto expected =
        yuv_of({color.red * gains.red, color.green * gains.green, color.blue * gains.blue});
    const std::array<double, 3> centred{before[0] - 16, before[1] - 128, before[2] - 128};
    for (int row = 0; row < 3; ++row) {
        double value = 0;
        for (int k = 0; k < 3; ++k) {
            value += matrix[row * 3 + k] * centred[k];
        }
        INFO("row " << row);
        CHECK(value + (row == 0 ? 16.0 : 128.0) == Approx(expected[row]).margin(1e-6));
    }
    const auto identity = yuv_matrix({});
    CHECK(identity[0] == Approx(1.0));
    CHECK(identity[4] == Approx(1.0));
    CHECK(identity[1] == Approx(0.0).margin(1e-12));
    const std::vector<ColorSpan> spans{{0.0, 10.0, gains}, {10.0, 20.0, {}}};
    CHECK(gains_at(spans, 5.0) == gains);
    CHECK(gains_at(spans, 15.0).identity());
    CHECK(gains_at(spans, 25.0).identity());
}
