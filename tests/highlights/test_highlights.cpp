// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Highlight marks, titles from file names, opening a game's highlights and the
// MakeHighlightVideo use case with in-memory fakes.

#include "highlights/application/game_highlights.hpp"
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
    std::map<std::string, StoredHighlights> saved;
    TitleLayout layout;
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

TEST_CASE("titles from file names") {
    CHECK(title_from_file_name("Weiss-Ohrstedt") == "Weiss - Ohrstedt");
    CHECK(title_from_file_name("weiss-belger_greta") == "Weiss - Belger Greta");
    CHECK(title_from_file_name("Weiss-Kroekel2") == "Weiss - Kroekel");
    CHECK(title_from_file_name("ttc-a-vs-b-c") == "Ttc A - B C");
    CHECK(title_from_file_name("2X6A4730").empty());
    CHECK(title_from_file_name("").empty());
}

TEST_CASE("swapping the players of a title") {
    CHECK(swap_players("Weiss - Ohrstedt") == "Ohrstedt - Weiss");
    CHECK(swap_players("Weiss - Ohrstedt\nKreisliga") == "Ohrstedt - Weiss\nKreisliga");
    CHECK(swap_players("Weiss vs Ohrstedt") == "Ohrstedt vs Weiss"); // older titles
    CHECK(swap_players("Finale") == "Finale");
}

TEST_CASE("title positions have stable names") {
    for (const TitlePosition position : kTitlePositions) {
        CHECK(parse_title_position(to_string(position)) == position);
    }
    CHECK_FALSE(parse_title_position("middle").has_value());
    CHECK_FALSE(Title{.text = "  \n", .shown = true}.visible());
    CHECK_FALSE(Title{.text = "A vs B", .shown = false}.visible());
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
        REQUIRE(renderer.clips[0].title);
        CHECK(renderer.clips[0].title->text == "A vs B");
    }
    SECTION("the match day: the match day highlights of every game") {
        const auto result = make.execute(request(std::nullopt, margins));
        CHECK(result.games == std::vector<std::string>{"a", "b"});
        REQUIRE(renderer.clips.size() == 2);
        CHECK(renderer.clips[0].video == "a.mp4");
        CHECK(renderer.clips[1].video == "b.mp4");
        CHECK_FALSE(renderer.clips[0].title); // titles only on request
    }
    SECTION("the match day with each game's title over its clips, all in the same layout") {
        store.saved["b"].title.size = 0.1;
        store.layout = {.size = 0.04, .position = TitlePosition::BottomRight};
        (void)make.execute(request(std::nullopt, margins, true));
        REQUIRE(renderer.clips[0].title);
        REQUIRE(renderer.clips[1].title);
        CHECK(renderer.clips[1].title->text == "C vs D");
        for (const Clip& clip : renderer.clips) {
            CHECK(clip.title->size == 0.04);
            CHECK(clip.title->position == TitlePosition::BottomRight);
        }
    }
    SECTION("a game video keeps the game's own layout") {
        store.layout = {.size = 0.04, .position = TitlePosition::TopRight};
        (void)make.execute(request("a", margins));
        CHECK(renderer.clips[0].title->position == TitlePosition::BottomCenter); // the default
    }
    SECTION("nothing marked") {
        store.saved["b"].rallies = {rally(60, 119)};
        CHECK_THROWS_AS(make.execute(request("b", margins)), std::invalid_argument);
    }
}
