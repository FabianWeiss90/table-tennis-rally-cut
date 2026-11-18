// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Highlight videos from clips generated with the ffmpeg command-line tool (skipped if it is not
// installed): the stored marks, two videos with different size, frame rate and sound, the
// cross-fade and a title.

#include "highlights/application/make_highlight_video.hpp"
#include "highlights/infrastructure/csv_highlight_store.hpp"
#include "highlights/infrastructure/ffmpeg_highlight_renderer.hpp"
#include "highlights/infrastructure/title_painter.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>

using namespace ttrally;
using namespace ttrally::highlights;

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

struct Written {
    std::int64_t frames = 0;
    int width = 0;
    double audio_s = 0.0;
};

Written inspect(const std::filesystem::path& path) {
    media::FfmpegMediaProbe probe;
    const auto info = probe.probe(path);
    REQUIRE(info.video);
    REQUIRE(info.audio);
    media::FfmpegMediaReader reader;
    const auto content =
        reader.read(path, {.audio_sample_rate = 8000, .video_stream_index = info.video->index});
    return {content.video_timestamps->frame_count(), info.video->width,
            static_cast<double>(content.audio->samples.size()) / 8000.0};
}

HighlightRally rally(std::int64_t start, std::int64_t end, bool game, bool day) {
    return {.start_frame = start, .end_frame = end, .game = game, .day = day};
}

} // namespace

TEST_CASE("highlight videos of a game and of a match day") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const auto day = std::filesystem::temp_directory_path() /
                     std::format("ttrally_highlights_test_{:08x}", device());
    const auto highlights_dir = day / "ttrally-data" / "highlights";
    std::filesystem::create_directories(highlights_dir);
    // a: 8 s, 320x180 at 60 fps, 48 kHz stereo; b: 6 s, 640x360 at 30 fps, 44.1 kHz mono
    const auto a = day / "Weiss-Ohrstedt.mp4";
    const auto b = day / "Weiss-Becker.mp4";
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate=60 "
                            "-f lavfi -i sine=frequency=440:sample_rate=48000 -ac 2 -t 8 "
                            "-c:v mpeg4 -q:v 4 -g 60 -c:a aac \"{}\"",
                            a.string())));
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=640x360:rate=30 "
                            "-f lavfi -i sine=frequency=880:sample_rate=44100 -t 6 "
                            "-c:v mpeg4 -q:v 4 -g 30 -c:a aac \"{}\"",
                            b.string())));
    CsvHighlightStore store(highlights_dir);
    const bool font = !default_title_font().empty();
    // a: 1-2 s (game and match day), 5-6 s (game); b: 1-2 s (match day)
    store.save("weiss-ohrstedt",
               {.video = a,
                .rallies = {rally(60, 119, true, true), rally(300, 359, true, false)},
                .coupled = true,
                .title = {.text = "Weiss vs Ohrstedt\nKreisliga", .shown = font}});
    store.save("weiss-becker", {.video = b,
                                .rallies = {rally(30, 59, false, true)},
                                .coupled = false,
                                .title = {.text = "Weiss vs Becker", .shown = font}});
    const auto stored = store.load("weiss-ohrstedt");
    REQUIRE(stored);
    CHECK(stored->video == a.lexically_normal());
    CHECK(stored->title.text == "Weiss vs Ohrstedt\nKreisliga");
    CHECK(store.match_day_layout() == TitleLayout{});
    store.save_match_day_layout({.size = 0.08, .position = TitlePosition::BottomRight});
    CHECK(store.match_day_layout() ==
          TitleLayout{.size = 0.08, .position = TitlePosition::BottomRight});
    CHECK(store.games() == std::vector<std::string>{"weiss-becker", "weiss-ohrstedt"});

    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader reader;
    FfmpegHighlightRenderer renderer;
    SilentProgressReporter progress;
    MakeHighlightVideo make(probe, reader, store, renderer, progress);
    const cutting::Margins margins{.before_s = 0.5, .after_s = 0.5};
    const RenderOptions render{.crossfade_s = 0.5, .encoding = {.encoder = "auto"}, .font = {}};

    SECTION("a game: two clips of 2 s with a cross-fade of 0.5 s") {
        const auto out = day / "game.mp4";
        const auto result = make.execute(
            {.video_id = "weiss-ohrstedt", .output = out, .margins = margins, .render = render});
        CHECK(result.clips == 2);
        CHECK(result.render.duration_s == Catch::Approx(3.5));
        const Written video = inspect(out);
        CHECK(video.frames == 210); // 120 + 120 - 30
        CHECK(video.width == 320);
        CHECK(video.audio_s == Catch::Approx(3.5).margin(0.05));
    }
    SECTION("the match day: clips of both videos in the first one's format") {
        const auto out = day / "day.mp4";
        const auto result = make.execute({.video_id = std::nullopt,
                                          .output = out,
                                          .margins = margins,
                                          .rally_titles = true,
                                          .render = render});
        CHECK(result.games == std::vector<std::string>{"weiss-becker", "weiss-ohrstedt"});
        const Written video = inspect(out);
        CHECK(video.width == 640); // weiss-becker comes first
        CHECK(video.frames == 105); // 30 fps: 60 + 60 - 15
        CHECK(video.audio_s == Catch::Approx(3.5).margin(0.05));
    }
    SECTION("hard cuts") {
        const auto out = day / "cuts.mp4";
        RenderOptions hard = render;
        hard.crossfade_s = 0.0;
        (void)make.execute(
            {.video_id = "weiss-ohrstedt", .output = out, .margins = margins, .render = hard});
        CHECK(inspect(out).frames == 240);
    }
    std::filesystem::remove_all(day);
}

TEST_CASE("titles are rendered with an outline") {
    const auto font_file = default_title_font();
    if (font_file.empty()) {
        SKIP("no system font found");
    }
    const TitleFont font(font_file);
    const TitleMask one = font.render("Weiss vs Ohrstedt", 40, TextAlign::Left);
    const TitleMask two = font.render("Weiss vs Ohrstedt\nÄ Ö Ü ß", 40, TextAlign::Center);
    CHECK(one.width > 200);
    CHECK(two.height > one.height + 30);
    CHECK(std::ranges::max(one.text) > 200);
    // The outline covers every letter pixel and more
    CHECK(std::ranges::count_if(one.outline, [](auto v) { return v > 0; }) >
          std::ranges::count_if(one.text, [](auto v) { return v > 0; }));

    // Painted on a mid-grey full-range picture: white letters, black outline, grey elsewhere
    constexpr int kWidth = 640;
    constexpr int kHeight = 360;
    std::vector<std::uint8_t> y(kWidth * kHeight, 128);
    std::vector<std::uint8_t> u(kWidth * kHeight / 4, 100);
    std::vector<std::uint8_t> v(kWidth * kHeight / 4, 150);
    paint_title({.y = y.data(), .u = u.data(), .v = v.data(), .y_stride = kWidth,
                 .uv_stride = kWidth / 2, .width = kWidth, .height = kHeight, .full_range = true},
                one, TitlePosition::BottomRight);
    CHECK(std::ranges::max(y) == 255);
    CHECK(std::ranges::min(y) == 0);
    CHECK(y[0] == 128); // top left untouched
    CHECK(std::ranges::find(u, 128) != u.end()); // grey under the title
}
