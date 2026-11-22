// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Highlight videos from clips generated with the ffmpeg command-line tool (skipped if it is not
// installed): the stored marks, two videos with different size, frame rate and sound, the
// cross-fade and a title.

#include "highlights/application/make_highlight_video.hpp"
#include "highlights/infrastructure/csv_highlight_store.hpp"
#include "highlights/infrastructure/ffmpeg_highlight_renderer.hpp"
#include "highlights/infrastructure/system_title_fonts.hpp"
#include "highlights/infrastructure/title_composer.hpp"
#include "highlights/infrastructure/title_painter.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <chrono>
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
                            "-c:v mpeg4 -q:v 4 -g 60 -c:a aac "
                            "-metadata creation_time=2026-11-21T16:30:00Z \"{}\"",
                            a.string())));
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=640x360:rate=30 "
                            "-f lavfi -i sine=frequency=880:sample_rate=44100 -t 6 "
                            "-c:v mpeg4 -q:v 4 -g 30 -c:a aac "
                            "-metadata creation_time=2026-11-21T14:00:00Z \"{}\"",
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
                                .title = {.text = "Weiss vs Becker",
                                          .shown = font,
                                          .decoration = {
                                              .backdrop = {.shape = BackdropShape::Cloud},
                                              .shadow = {.shown = true},
                                              .glow = {.shown = true}}}});
    const auto stored = store.load("weiss-ohrstedt");
    REQUIRE(stored);
    CHECK(stored->video == a.lexically_normal());
    CHECK(stored->title.text == "Weiss vs Ohrstedt\nKreisliga");
    CHECK(store.match_day_layout() == TitleLayout{});
    store.save_match_day_layout({.size = 0.08, .position = TitlePosition::BottomRight});
    CHECK(store.match_day_layout() ==
          TitleLayout{.size = 0.08, .position = TitlePosition::BottomRight});
    // The match title shares the file with the layout; neither overwrites the other
    CHECK_FALSE(store.match_title());
    const Title match{.text = "Club - Guests\n3:9", .shown = true, .size = 0.07,
                      .position = TitlePosition::TopCenter, .color = {255, 200, 0},
                      .outline = {0, 0, 80}};
    store.save_match_title(match);
    store.save_match_day_layout({.size = 0.06, .position = TitlePosition::BottomLeft,
                                 .color = {1, 2, 3}, .outline = {4, 5, 6}});
    CHECK(store.match_title() == match);
    CHECK(store.match_day_layout() == TitleLayout{.size = 0.06,
                                                  .position = TitlePosition::BottomLeft,
                                                  .color = {1, 2, 3},
                                                  .outline = {4, 5, 6}});
    CHECK(store.title_font().empty());
    store.save_title_font("/fonts/Inter-SemiBold.ttf");
    CHECK(store.title_font() == std::filesystem::path("/fonts/Inter-SemiBold.ttf"));
    CHECK(store.match_title() == match); // untouched by the font
    store.save_title_font({});
    CHECK(store.title_font().empty());
    StoredHighlights coloured = *stored;
    coloured.title.color = {12, 34, 56};
    coloured.title.outline = {200, 100, 0};
    coloured.title.outlined = true;
    store.save("weiss-ohrstedt", coloured);
    CHECK(store.load("weiss-ohrstedt")->title == coloured.title);
    // Decorations are kept with the title and with the match day layout
    StoredHighlights decorated = *stored;
    decorated.title.decoration = {
        .backdrop = {.shape = BackdropShape::Svg, .svg = "banner.svg", .color = {9, 8, 7},
                     .opacity = 45, .per_line = true, .seed = 123456789U, .padding = 0.5,
                     .corner = 0.25, .softness = 0.125, .bordered = true, .border = {1, 2, 3},
                     .border_width = 0.1},
        .shadow = {.shown = true, .color = {4, 5, 6}, .opacity = 30, .offset = 0.2, .blur = 0.25},
        .glow = {.shown = true, .color = {7, 8, 9}, .opacity = 90, .size = 0.3}};
    store.save("weiss-ohrstedt", decorated);
    CHECK(store.load("weiss-ohrstedt")->title == decorated.title);
    TitleLayout day_layout = store.match_day_layout();
    day_layout.decoration.backdrop.shape = BackdropShape::Cloud;
    day_layout.decoration.glow.shown = true;
    store.save_match_day_layout(day_layout);
    CHECK(store.match_day_layout() == day_layout);
    store.save("weiss-ohrstedt", *stored);
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
            {.video_id = "weiss-ohrstedt", .output = out, .match_day_name = "", .margins = margins, .render = render});
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
                                          .match_day_name = "",
                                          .margins = margins,
                                          .rally_titles = true,
                                          .render = render});
        CHECK(result.games == std::vector<std::string>{"weiss-becker", "weiss-ohrstedt"});
        const Written video = inspect(out);
        CHECK(video.width == 640); // weiss-becker comes first: recorded first
        CHECK(video.frames == 105); // 30 fps: 60 + 60 - 15
        CHECK(video.audio_s == Catch::Approx(3.5).margin(0.05));
    }
    SECTION("the match day at a lower resolution: every clip shrunk to it") {
        const auto out = day / "small.mp4";
        RenderOptions small = render;
        small.encoding.max_height = 180; // weiss-becker is 640x360, weiss-ohrstedt 320x180
        const auto result = make.execute({.video_id = std::nullopt,
                                          .output = out,
                                          .match_day_name = "",
                                          .margins = margins,
                                          .rally_titles = true,
                                          .render = small,
                                          .resolution_in_name = true});
        CHECK(result.output == day / "small-180p.mp4");
        CHECK(inspect(result.output).width == 320);
    }
    SECTION("a slow motion right after its rally, zoomed, with stretched sound") {
        StoredHighlights game = *store.load("weiss-ohrstedt");
        game.rallies[0].slow = SlowMotion{.start_frame = 60,
                                          .end_frame = 89, // 0.5 s
                                          .factor = 2,
                                          .left = 0.25,
                                          .top = 0.25,
                                          .size = 0.5,
                                          .sound = true};
        store.save("weiss-ohrstedt", game);
        REQUIRE(store.load("weiss-ohrstedt")->rallies[0].slow == game.rallies[0].slow);
        const auto out = day / "slow.mp4";
        const auto result = make.execute({.video_id = "weiss-ohrstedt",
                                          .output = out,
                                          .match_day_name = "",
                                          .margins = margins,
                                          .render = render});
        CHECK(result.clips == 3);
        // 2 s + 1 s (0.5 s twice as slow) + 2 s, minus two cross-fades of 0.5 s
        CHECK(result.render.duration_s == Catch::Approx(4.0));
        const Written video = inspect(out);
        CHECK(video.frames == 240);
        CHECK(video.audio_s == Catch::Approx(4.0).margin(0.05));
        const auto without = make.execute({.video_id = "weiss-ohrstedt",
                                           .output = day / "without.mp4",
                                           .match_day_name = "",
                                           .margins = margins,
                                           .render = render,
                                           .slow_motions = false});
        CHECK(without.clips == 2);
    }
    SECTION("the games in the order they were recorded, unless another order is chosen") {
        media::FfmpegMediaProbe probe;
        using namespace std::chrono;
        const auto at = [](int hour, int minute) {
            return duration_cast<seconds>(
                       (sys_days{year{2026} / 11 / 21} + hours{hour} + minutes{minute})
                           .time_since_epoch())
                .count();
        };
        CHECK(probe.probe(a).recorded_at_s == at(16, 30));
        CHECK(probe.probe(b).recorded_at_s == at(14, 0));
        store.save_rally_order("", {{"weiss-ohrstedt", 60}, {"weiss-becker", 30}});
        CHECK(store.rally_order("") ==
              std::vector<RallyKey>{{"weiss-ohrstedt", 60}, {"weiss-becker", 30}});
        CHECK(store.rally_order("weiss-ohrstedt").empty());
        const auto out = day / "ordered.mp4";
        const auto result = make.execute({.video_id = std::nullopt,
                                          .output = out,
                                          .match_day_name = "",
                                          .margins = margins,
                                          .rally_titles = true,
                                          .render = render});
        CHECK(result.games == std::vector<std::string>{"weiss-ohrstedt", "weiss-becker"});
        CHECK(inspect(out).width == 320); // weiss-ohrstedt chosen first
    }
    SECTION("hard cuts") {
        const auto out = day / "cuts.mp4";
        RenderOptions hard = render;
        hard.crossfade_s = 0.0;
        (void)make.execute(
            {.video_id = "weiss-ohrstedt", .output = out, .match_day_name = "", .margins = margins, .render = hard});
        CHECK(inspect(out).frames == 240);
    }
    std::filesystem::remove_all(day);
}

namespace {

/// The title in its colours on a full-range picture.
PaintableTitle drawn(const TitleMask& letters, const Title& title) {
    return paintable(compose_title(letters, title, 40, nullptr), true);
}

} // namespace

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
                drawn(one, Title{.text = "", .position = TitlePosition::BottomRight,
                                 .outlined = true}),
                TitlePosition::BottomRight);
    CHECK(std::ranges::max(y) == 255);
    CHECK(std::ranges::min(y) == 0);
    CHECK(y[0] == 128); // top left untouched
    CHECK(std::ranges::find(u, 128) != u.end()); // grey under the title

    // In colour: red letters with a blue outline
    std::ranges::fill(y, 128);
    std::ranges::fill(u, 128);
    std::ranges::fill(v, 128);
    paint_title({.y = y.data(), .u = u.data(), .v = v.data(), .y_stride = kWidth,
                 .uv_stride = kWidth / 2, .width = kWidth, .height = kHeight, .full_range = true},
                drawn(one, Title{.text = "", .position = TitlePosition::TopLeft,
                                 .color = {255, 0, 0}, .outlined = true, .outline = {0, 0, 255}}),
                TitlePosition::TopLeft);
    const Yuv red = to_yuv({255, 0, 0}, true);
    const Yuv blue = to_yuv({0, 0, 255}, true);
    CHECK(red.v > 200);
    CHECK(blue.u > 200);
    CHECK(std::ranges::find(v, red.v) != v.end());
    CHECK(std::ranges::find(u, blue.u) != u.end());
    CHECK(to_yuv(kWhite, false).y == 235);
    CHECK(to_yuv(kBlack, false).y == 16);
    CHECK(to_yuv(kWhite, true).u == 128);

    // Without a border only the letters are painted
    std::ranges::fill(y, 128);
    paint_title({.y = y.data(), .u = u.data(), .v = v.data(), .y_stride = kWidth,
                 .uv_stride = kWidth / 2, .width = kWidth, .height = kHeight, .full_range = true},
                drawn(one, Title{.text = "", .position = TitlePosition::TopLeft}),
                TitlePosition::TopLeft);
    CHECK(std::ranges::max(y) == 255);
    CHECK(std::ranges::min(y) == 128);
}

TEST_CASE("installed fonts are listed by name and write titles") {
    const auto font_file = default_title_font();
    if (font_file.empty()) {
        SKIP("no system font found");
    }
    SystemTitleFonts fonts({font_file.parent_path()});
    const std::vector<FontChoice> installed = fonts.installed();
    REQUIRE_FALSE(installed.empty());
    CHECK(std::ranges::is_sorted(installed, {}, &FontChoice::name));
    const auto found = std::ranges::find(installed, font_file, &FontChoice::file);
    REQUIRE(found != installed.end());
    CHECK_FALSE(found->name.empty());
    CHECK(fonts.fallback().file == font_file);
    const TitleImage image =
        fonts.render({}, Title{.text = "Weiss - Ohrstedt", .shown = true}, 40, {});
    CHECK(image.width > 200);
    CHECK(image.height >= 40);
    // A font that no longer exists falls back to the default, as in the video
    CHECK(fonts.render("/no/such/font.ttf", Title{.text = "x", .shown = true}, 40, {}).width > 0);
}
