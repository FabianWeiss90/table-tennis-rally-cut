// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Highlight marks, titles from file names, opening a game's highlights and the
// MakeHighlightVideo use case with in-memory fakes.

#include "highlights/application/game_highlights.hpp"
#include "highlights/application/highlight_length.hpp"
#include "highlights/domain/highlight_selection.hpp"
#include "highlights/application/make_highlight_video.hpp"
#include "support/fake_media.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <map>
#include <stdexcept>

using namespace ttrally;
using namespace ttrally::highlights;
using Catch::Approx;

namespace {

constexpr int kFps = 60;
constexpr int kFrames = 1200; // 20 s

HighlightRally rally(std::int64_t start, std::int64_t end, bool game = false, bool day = false) {
    return {.start_frame = start, .end_frame = end, .game = game, .day = day};
}

class MemoryStore final : public HighlightStore {
  public:
    std::vector<std::string> games() override {
        std::vector<std::string> ids;
        for (const auto& [id, stored] : saved) {
            ids.push_back(id);
        }
        return ids;
    }
    std::optional<StoredHighlights> load(const std::string& video_id) override {
        const auto found = saved.find(video_id);
        return found == saved.end() ? std::nullopt : std::optional(found->second);
    }
    void save(const std::string& video_id, const StoredHighlights& highlights) override {
        saved[video_id] = highlights;
    }
    TitleLayout match_day_layout() override { return layout; }
    void save_match_day_layout(const TitleLayout& changed) override { layout = changed; }
    std::optional<Title> match_title() override { return match; }
    void save_match_title(const Title& changed) override { match = changed; }
    std::filesystem::path title_font() override { return font; }
    void save_title_font(const std::filesystem::path& changed) override { font = changed; }
    std::filesystem::path font;
    std::map<std::string, StoredHighlights> saved;
    TitleLayout layout;
    std::optional<Title> match;
};

class MemoryRallies final : public RallySource {
  public:
    std::optional<RallyList> rallies(const std::string&) override { return list; }
    std::optional<RallyList> list;
};

class RecordingRenderer final : public HighlightRenderer {
  public:
    cutting::CutResult render(std::span<const Clip> rendered, const std::filesystem::path& output,
                              const RenderOptions&, ProgressReporter&) override {
        clips.assign(rendered.begin(), rendered.end());
        written = output;
        return {"fake", 0.0, 0.0};
    }
    std::vector<Clip> clips;
    std::filesystem::path written;
};

test::FakeFile sixty_fps_video() {
    test::FakeFile file;
    file.info.video = media::VideoStreamInfo{};
    file.info.video->index = 0;
    file.info.video->avg_frame_rate = {kFps, 1};
    file.timestamps.time_base = {1, kFps};
    for (int i = 0; i < kFrames; ++i) {
        file.timestamps.pts.push_back(i);
    }
    return file;
}

MakeHighlightsRequest request(std::optional<std::string> video_id, cutting::Margins margins,
                              bool rally_titles = false) {
    return {.video_id = std::move(video_id),
            .output = "out.mp4",
            .match_day_name = "",
            .margins = margins,
            .rally_titles = rally_titles,
            .render = {}};
}

} // namespace

TEST_CASE("preselection marks the longest rallies for game and match day") {
    HighlightSelection selection(
        {rally(500, 559), rally(0, 99), rally(200, 259), rally(300, 399, true, true)});
    REQUIRE(selection.rallies().front().start_frame == 0); // sorted by start

    selection.preselect(2);
    // 0-99 and 300-399 are equally long (100 frames); 200-259 and 500-559 shorter
    CHECK(selection.rallies()[0].game);
    CHECK(selection.rallies()[0].day);
    CHECK_FALSE(selection.rallies()[1].game);
    CHECK(selection.rallies()[2].game);
    CHECK_FALSE(selection.rallies()[3].game);
    CHECK(selection.game_count() == 2);
    CHECK(selection.day_count() == 2);

    SECTION("equal lengths: the earlier first") {
        selection.preselect(1);
        CHECK(selection.rallies()[0].game);
        CHECK_FALSE(selection.rallies()[2].game);
    }
    SECTION("more than there are") {
        selection.preselect(10);
        CHECK(selection.game_count() == 4);
    }
    SECTION("clear removes every mark") {
        selection.clear();
        CHECK(selection.game_count() == 0);
        CHECK(selection.day_count() == 0);
    }
}

TEST_CASE("coupled marks: a match day highlight is always a game highlight") {
    HighlightSelection selection({rally(0, 9), rally(20, 29)});
    selection.set_day(0, true);
    CHECK(selection.rallies()[0].game);
    selection.set_game(1, true);
    CHECK_FALSE(selection.rallies()[1].day); // not the other way round
    selection.set_game(0, false);
    CHECK_FALSE(selection.rallies()[0].day);

    SECTION("uncoupled, the marks are independent") {
        selection.set_coupled(false);
        selection.set_day(0, true);
        CHECK_FALSE(selection.rallies()[0].game);
        selection.set_game(1, false);
        selection.set_day(1, true);
        selection.set_game(1, false);
        CHECK(selection.rallies()[1].day);

        SECTION("coupling again marks the match day highlights for the game") {
            selection.set_coupled(true);
            CHECK(selection.rallies()[0].game);
            CHECK(selection.rallies()[1].game);
        }
    }
    CHECK_THROWS_AS(selection.set_game(2, true), std::out_of_range);
}

TEST_CASE("marks are carried over to changed rallies by their overlap") {
    const std::vector<HighlightRally> saved{rally(100, 199, true, true), rally(400, 499, true)};
    const auto rallies = carry_over_marks(
        {rally(90, 210), rally(300, 350), rally(450, 600), rally(480, 900)}, saved);
    CHECK(rallies[0] == rally(90, 210, true, true)); // the old rally, slightly wider
    CHECK(rallies[1] == rally(300, 350));            // new
    CHECK(rallies[2] == rally(450, 600, true));      // shares 50 of the shorter one's 100
    CHECK(rallies[3] == rally(480, 900));            // shares only 20
}

TEST_CASE("the length of a highlight video follows the marks, margins and cross-fades") {
    const std::vector<HighlightRally> rallies{
        rally(600, 1199, true, true),  // 10 s to 20 s
        rally(1260, 1559, true),       // 21 s to 26 s: margins overlap the first one
        rally(6000, 6299, true, true), // 100 s to 105 s
    };
    const cutting::Margins margins{.before_s = 1.0, .after_s = 1.5};
    const auto game = clip_lengths(rallies, HighlightVideo::Game, margins, kFps);
    REQUIRE(game.size() == 2);
    CHECK(game[0] == Approx(27.5 - 9.0)); // merged: 9 s to 27.5 s
    CHECK(game[1] == Approx(7.5));
    const auto day = clip_lengths(rallies, HighlightVideo::MatchDay, margins, kFps);
    REQUIRE(day.size() == 2);
    CHECK(day[0] == Approx(12.5));
    CHECK(joined_length(game, 0.0) == Approx(26.0));
    CHECK(joined_length(game, 0.5) == Approx(25.5));
    // A fade takes at most half of either clip
    const std::vector<double> short_clips{1.0, 10.0, 4.0};
    CHECK(joined_length(short_clips, 3.0) == Approx(15.0 - 0.5 - 2.0));
    CHECK(joined_length({}, 0.5) == 0.0);
    CHECK(clip_lengths(rallies, HighlightVideo::Game, margins, 0.0).empty());
    const std::vector<HighlightRally> start{rally(0, 59, true)};
    CHECK(clip_lengths(start, HighlightVideo::Game, margins, kFps)[0] == Approx(2.5));
}

TEST_CASE("the longest rallies are marked for the match day, game marks stay") {
    HighlightSelection selection({rally(0, 99, true), rally(200, 499), rally(600, 799, true, true),
                                  rally(900, 1199)});
    selection.mark_longest_for_day(2); // 300 frames twice: the earlier first, then 200
    CHECK(selection.rallies()[1].day);
    CHECK(selection.rallies()[3].day);
    CHECK_FALSE(selection.rallies()[2].day); // unmarked for the match day ...
    CHECK(selection.rallies()[2].game);      // ... but still a game highlight
    CHECK(selection.rallies()[0].game);
    CHECK(selection.rallies()[1].game); // coupled: a match day highlight is a game highlight
    CHECK(selection.day_count() == 2);
}

TEST_CASE("the match day video takes as many rallies from every game as fit") {
    const cutting::Margins margins{.before_s = 1.0, .after_s = 1.0};
    // Rallies of 4, 3, 2 and 1 s, far apart: clips of 6, 5, 4 and 3 s
    const auto game_of = [] {
        HighlightSelection selection({rally(600, 839), rally(1600, 1779), rally(2600, 2719),
                                      rally(3600, 3659)});
        return GameMarks{selection, kFps};
    };
    const std::vector<GameMarks> games{game_of(), game_of(), game_of()};
    // 1 per game: 3 x 6 s = 18 s; 2 per game: 3 x 11 s = 33 s; 3 per game: 45 s
    DayFit fit = fit_match_day(games, 40.0, margins, 0.0);
    CHECK(fit.per_game == 2);
    CHECK(fit.length_s == Approx(33.0));
    // 37 s left over would hold another rally of one game, but every game keeps two
    fit = fit_match_day(games, 44.0, margins, 0.0);
    CHECK(fit.per_game == 2);
    // Cross-fades shorten the video: 6 clips with 5 fades of 0.5 s make 33 - 2.5 s
    CHECK(fit_match_day(games, 30.5, margins, 0.5).per_game == 2);
    CHECK(fit_match_day(games, 30.4, margins, 0.5).per_game == 1);
    CHECK(fit_match_day(games, 10.0, margins, 0.0).per_game == 0);
    // A game with fewer rallies gives all it has
    std::vector<GameMarks> uneven = games;
    uneven.push_back({HighlightSelection({rally(600, 839)}), kFps});
    fit = fit_match_day(uneven, 59.0, margins, 0.0); // 4 per game: 3 x 18 + 6 = 60 s
    CHECK(fit.per_game == 3);
    CHECK(fit.length_s == Approx(3 * 15.0 + 6.0));
}

TEST_CASE("titles from file names") {
    CHECK(title_from_file_name("Weiss-Ohrstedt") == "Weiss - Ohrstedt");
    CHECK(title_from_file_name("weiss-belger_greta") == "Weiss - Belger Greta");
    CHECK(title_from_file_name("Weiss-Kroekel2") == "Weiss - Kroekel");
    CHECK(title_from_file_name("ttc-a-vs-b-c") == "Ttc A - B C");
    CHECK(title_from_file_name("2X6A4730").empty());
    CHECK(title_from_file_name("").empty());
}

TEST_CASE("titles of doubles from file names") {
    CHECK(title_from_file_name("Doppel-Weiss_Becker-Mueller_Schmidt") ==
          "Weiss/Becker - Mueller/Schmidt");
    CHECK(title_from_file_name("doppel-weiss_becker-mueller_schmidt") ==
          "Weiss/Becker - Mueller/Schmidt");
    CHECK(title_from_file_name("Doubles-Weiss_Becker-Mueller_Schmidt2") ==
          "Weiss/Becker - Mueller/Schmidt");
    CHECK(title_from_file_name("doubles-A_B-C_D") == "A/B - C/D");
    CHECK(title_from_file_name("Doppel2-A_B-C_D") == "A/B - C/D");
    CHECK(swap_players("Weiss/Becker - Mueller/Schmidt") == "Mueller/Schmidt - Weiss/Becker");
    // Not a doubles mark: read as singles
    CHECK(title_from_file_name("Doppler-Weiss") == "Doppler - Weiss");
    CHECK(title_from_file_name("Doppel-Weiss") == "Doppel - Weiss"); // no pairs: as singles
}

TEST_CASE("swapping the players of a title") {
    CHECK(swap_players("Weiss - Ohrstedt") == "Ohrstedt - Weiss");
    CHECK(swap_players("Weiss - Ohrstedt\nKreisliga") == "Ohrstedt - Weiss\nKreisliga");
    CHECK(swap_players("Weiss vs Ohrstedt") == "Ohrstedt vs Weiss"); // older titles
    CHECK(swap_players("Finale") == "Finale");
}

TEST_CASE("the match from the match day folder") {
    CHECK(match_from_folder_name("2026-11-14-WolmirstedtII-Bismark") == "WolmirstedtII - Bismark");
    CHECK(match_from_folder_name("2026-10-31-bismark-groeningen") == "Bismark - Groeningen");
    CHECK(match_from_folder_name("Bismark-Groeningen") == "Bismark - Groeningen");
    CHECK(match_from_folder_name("2026-10-03-Club").empty());
    const Title title = default_match_title("2026-11-14-A-B");
    CHECK(title.text == "A - B");
    CHECK(title.position == TitlePosition::TopCenter);
}

TEST_CASE("title colours as hex") {
    CHECK(to_hex({255, 128, 0}) == "#ff8000");
    CHECK(parse_hex("#ff8000") == Rgb{255, 128, 0});
    CHECK(parse_hex("00FF7f") == Rgb{0, 255, 127});
    CHECK_FALSE(parse_hex("#ff80"));
    CHECK_FALSE(parse_hex("#gg0000"));
}

TEST_CASE("title positions have stable names") {
    for (const TitlePosition position : kTitlePositions) {
        CHECK(parse_title_position(to_string(position)) == position);
    }
    CHECK_FALSE(parse_title_position("middle").has_value());
    CHECK_FALSE(Title{.text = "  \n", .shown = true}.visible());
    CHECK_FALSE(Title{.text = "A vs B", .shown = false}.visible());
}

TEST_CASE("title backdrops have stable names and change the look") {
    for (const BackdropShape shape : kBuiltInBackdrops) {
        CHECK(parse_backdrop_shape(to_string(shape)) == shape);
    }
    CHECK(parse_backdrop_shape("svg") == BackdropShape::Svg);
    CHECK_FALSE(parse_backdrop_shape("star").has_value());
    CHECK(uses_seed(BackdropShape::Cloud));
    CHECK(uses_seed(BackdropShape::DoubleStrip));
    CHECK_FALSE(uses_seed(BackdropShape::RoundedBox));
    CHECK(next_seed(1) != 1);
    CHECK(next_seed(1) == next_seed(1));

    CHECK_FALSE(Backdrop{}.visible());
    CHECK(Backdrop{.shape = BackdropShape::Cloud}.visible());
    CHECK_FALSE(Backdrop{.shape = BackdropShape::Svg}.visible()); // no file chosen
    CHECK_FALSE(Backdrop{.shape = BackdropShape::Pill, .opacity = 0}.visible());
    CHECK(Backdrop{.shape = BackdropShape::Pill, .opacity = 0, .bordered = true}.visible());

    Title title{.text = "A - B", .shown = true};
    const std::string plain = look_key(title);
    title.decoration.backdrop.seed = 2;
    CHECK(look_key(title) != plain);
    title.decoration.backdrop.seed = 1;
    title.decoration.glow.shown = true;
    CHECK(look_key(title) != plain);

    // A layout carries the decoration
    const TitleLayout layout = TitleLayout::of(title);
    CHECK(layout.decoration == title.decoration);
    CHECK(layout.applied_to(Title{.text = "C - D"}).decoration == title.decoration);
}

TEST_CASE("opening a game's highlights") {
    MemoryRallies rallies;
    MemoryStore store;
    const std::filesystem::path video = "/day/Weiss-Ohrstedt.MP4";

    SECTION("without rallies there is nothing to choose from") {
        CHECK_FALSE(open_game_highlights(rallies, store, "weiss-ohrstedt", video, 3).has_value());
    }

    rallies.list = RallyList{.rallies = {rally(0, 99), rally(200, 239), rally(300, 449)},
                             .fps = 59.94,
                             .source = "detections"};
    SECTION("the first time: preselected, title from the file name, saved") {
        const auto game = open_game_highlights(rallies, store, "weiss-ohrstedt", video, 2);
        REQUIRE(game);
        CHECK(game->selection.game_count() == 2);
        CHECK(game->title.text == "Weiss - Ohrstedt");
        CHECK(game->fps == 59.94);
        REQUIRE(store.saved.contains("weiss-ohrstedt"));
        CHECK(store.saved["weiss-ohrstedt"].video == video);
    }
    SECTION("later: the saved marks and title on the current rallies") {
        store.saved["weiss-ohrstedt"] = {.video = video,
                                         .rallies = {rally(200, 239, false, true)},
                                         .coupled = false,
                                         .title = {.text = "Finale", .shown = false}};
        const auto game = open_game_highlights(rallies, store, "weiss-ohrstedt", video, 2);
        REQUIRE(game);
        CHECK(game->selection.rallies().size() == 3);
        CHECK(game->selection.rallies()[1] == rally(200, 239, false, true));
        CHECK_FALSE(game->selection.coupled());
        CHECK(game->title.text == "Finale");
        CHECK(store.saved["weiss-ohrstedt"].rallies.size() == 3); // the current rallies
    }
}

TEST_CASE("MakeHighlightVideo builds the clips of the marked rallies") {
    test::FakeMediaLibrary library;
    library.add("a.mp4", sixty_fps_video());
    library.add("b.mp4", sixty_fps_video());
    MemoryStore store;
    const Title title_a{.text = "A vs B", .shown = true};
    store.saved["a"] = {.video = "a.mp4",
                        .rallies = {rally(120, 179, true, true), rally(240, 299, true),
                                    rally(600, 659, true)},
                        .coupled = true,
                        .title = title_a};
    store.saved["b"] = {.video = "b.mp4",
                        .rallies = {rally(60, 119, false, true)},
                        .coupled = false,
                        .title = {.text = "C vs D", .shown = true}};
    RecordingRenderer renderer;
    SilentProgressReporter progress;
    MakeHighlightVideo make(library, library, store, renderer, progress);
    const cutting::Margins margins{.before_s = 1.0, .after_s = 1.0};

    SECTION("a game: its game highlights with its title, overlapping margins merged") {
        const auto result = make.execute(request("a", margins));
        CHECK(result.rallies == 3);
        REQUIRE(renderer.clips.size() == 2);
        // 2-3 s and 4-5 s with 1 s margins overlap: 1-6 s; 10-11 s: 9-12 s
        CHECK(renderer.clips[0].start_s == Approx(1.0));
        CHECK(renderer.clips[0].end_s == Approx(6.0));
        CHECK(renderer.clips[1].start_s == Approx(9.0));
        CHECK(renderer.clips[1].end_s == Approx(12.0));
        REQUIRE(renderer.clips[0].titles.size() == 1);
        CHECK(renderer.clips[0].titles[0].text == "A vs B");
    }
    SECTION("the match day: the match day highlights of every game") {
        const auto result = make.execute(request(std::nullopt, margins));
        CHECK(result.games == std::vector<std::string>{"a", "b"});
        REQUIRE(renderer.clips.size() == 2);
        CHECK(renderer.clips[0].video == "a.mp4");
        CHECK(renderer.clips[1].video == "b.mp4");
        CHECK(renderer.clips[0].titles.empty()); // players only on request
    }
    SECTION("the match day with each game's title over its clips, all in the same layout") {
        store.saved["b"].title.size = 0.1;
        store.layout = {.size = 0.04, .position = TitlePosition::BottomRight};
        (void)make.execute(request(std::nullopt, margins, true));
        REQUIRE(renderer.clips[0].titles.size() == 1);
        REQUIRE(renderer.clips[1].titles.size() == 1);
        CHECK(renderer.clips[1].titles[0].text == "C vs D");
        for (const Clip& clip : renderer.clips) {
            CHECK(clip.titles[0].size == 0.04);
            CHECK(clip.titles[0].position == TitlePosition::BottomRight);
        }
    }
    SECTION("a game video keeps the game's own layout") {
        store.layout = {.size = 0.04, .position = TitlePosition::TopRight};
        (void)make.execute(request("a", margins));
        CHECK(renderer.clips[0].titles[0].position == TitlePosition::BottomCenter); // default
    }
    SECTION("the match over every clip of both videos, first") {
        store.match = Title{.text = "Club - Guests", .shown = true, .size = 0.06,
                            .position = TitlePosition::TopCenter, .color = {255, 200, 0},
                            .outline = {0, 0, 80}};
        (void)make.execute(request("a", margins));
        REQUIRE(renderer.clips[0].titles.size() == 2);
        CHECK(renderer.clips[0].titles[0] == *store.match);
        CHECK(renderer.clips[0].titles[1].text == "A vs B");
        (void)make.execute(request(std::nullopt, margins));
        for (const Clip& clip : renderer.clips) {
            REQUIRE(clip.titles.size() == 1);
            CHECK(clip.titles[0] == *store.match);
        }
    }
    SECTION("the match from the folder name if none was saved, or none if hidden") {
        MakeHighlightsRequest named = request("a", margins);
        named.match_day_name = "2026-11-14-Club-Guests";
        (void)make.execute(named);
        REQUIRE(renderer.clips[0].titles.size() == 2);
        CHECK(renderer.clips[0].titles[0].text == "Club - Guests");
        CHECK(renderer.clips[0].titles[0].position == TitlePosition::TopCenter);
        store.match = Title{.text = "Club - Guests", .shown = false};
        (void)make.execute(named);
        CHECK(renderer.clips[0].titles.size() == 1);
    }
    SECTION("the match day layout carries the colours") {
        store.layout = {.size = 0.05, .position = TitlePosition::BottomCenter,
                        .color = {10, 20, 30}, .outline = {200, 210, 220}};
        (void)make.execute(request(std::nullopt, margins, true));
        CHECK(renderer.clips[0].titles[0].color == Rgb{10, 20, 30});
        CHECK(renderer.clips[0].titles[0].outline == Rgb{200, 210, 220});
    }
    SECTION("nothing marked") {
        store.saved["b"].rallies = {rally(60, 119)};
        CHECK_THROWS_AS(make.execute(request("b", margins)), std::invalid_argument);
    }
}
