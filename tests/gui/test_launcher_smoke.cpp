// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Smoke test of the launcher window: opens it with SDL's invisible "offscreen" video driver,
// renders a few frames with in-memory workspace and runner and saves a screenshot.

#include "gui/launcher_app.hpp"
#include "media/application/media_error.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <utility>

using namespace ttrally;

namespace {

class OneVideo final : public launcher::WorkspaceScanner {
  public:
    launcher::WorkspaceSnapshot scan(const std::filesystem::path& library) override {
        launcher::WorkspaceSnapshot workspace;
        workspace.library = library.string();
        launcher::VideoEntry video;
        video.match_day = "2026-10-03 Club";
        video.original = "/lib/2026-10-03 Club/Match_FHD.mp4";
        video.video_id = "match";
        video.work_dir = "/lib/2026-10-03 Club/ttrally-data";
        video.cut_candidates = {"/lib/2026-10-03 Club/Liimba/Match_FHD.mp4"};
        video.aligned = true;
        video.annotation = launcher::AnnotationProgress::InProgress;
        video.scene = with_match ? "A" : "";
        workspace.videos = {video};
        if (with_match) {
            launcher::VideoEntry match = video;
            match.video_id = "kaether-baumgarten";
            match.original =
                "/lib/2026-10-03 Club/ttrally-data/matches/kaether-baumgarten.ffconcat";
            match.parts = {"/lib/2026-10-03 Club/Kaether-Baumgarten-1.MP4",
                           "/lib/2026-10-03 Club/Kaether-Baumgarten-2.MP4"};
            workspace.videos.push_back(match);
        }
        for (int i = 2; i <= extra_videos + 1; ++i) {
            video.video_id = std::format("match-{}", i);
            video.original = std::format("/lib/2026-10-03 Club/Match{}_FHD.mp4", i);
            video.detected = true;
            workspace.videos.push_back(video);
        }
        workspace.trainable = {video.key(), "2026-10-10 Away/other"};
        return workspace;
    }
    int extra_videos = 0; ///< more videos of the match day
    bool with_match = false; ///< a match of two files too
};

/// White balance settings in memory.
class MemoryWhiteBalance final : public cutting::WhiteBalanceStore {
  public:
    cutting::WhiteBalanceSettings load(const std::filesystem::path&) override { return saved; }
    void save(const std::filesystem::path&,
              const cutting::WhiteBalanceSettings& settings) override {
        saved = settings;
    }
    cutting::WhiteBalanceSettings saved{
        .files = {{"Match_FHD.mp4", cutting::RelativeRect{0.2, 0.2, 0.4, 0.3}, {}}},
        .reference = "Match_FHD.mp4",
        .temperature = 0.2,
        .tint = 0.0,
        .brightness = 0.0,
        .match_brightness = false};
};

/// Scenes in memory.
class MemoryScenes final : public launcher::SceneStore {
  public:
    std::map<std::string, std::string> load(const std::filesystem::path&) override { return {}; }
    void save(const std::filesystem::path&, const std::map<std::string, std::string>&) override {}
};

class IdleRunner final : public launcher::ProcessRunner {
  public:
    void start(const launcher::Invocation&) override {}
    bool running() const override { return false; }
    std::string take_output() override { return {}; }
    std::optional<int> exit_code() const override { return 0; }
    void cancel() override {}
};

/// A grey frame of the requested height.
class GreyPreview final : public launcher::VideoPreview {
  public:
    std::optional<media::VideoFrame> frame_at(const std::filesystem::path&, double fraction,
                                              int height) override {
        media::VideoFrame frame;
        frame.time_s = 600.0 * fraction;
        frame.size = {height * 16 / 9 / 2 * 2, height};
        frame.planes.assign(media::VideoFrame::bytes_for(frame.size), 128);
        ++requests;
        return frame;
    }
    std::atomic<int> requests = 0; // called by the preview and the thumbnail threads
};

/// Highlights of a match day in memory: 12 rallies of growing length for every game.
class MemoryHighlights final : public highlights::MatchDayHighlights {
  public:
    class Store final : public highlights::HighlightStore {
      public:
        std::vector<std::string> games() override { return {}; }
        std::optional<highlights::StoredHighlights> load(const std::string&) override {
            return saved;
        }
        void save(const std::string&, const highlights::StoredHighlights& stored) override {
            saved = stored;
        }
        highlights::TitleLayout match_day_layout() override { return {}; }
        void save_match_day_layout(const highlights::TitleLayout&) override {}
        std::optional<highlights::Title> match_title() override { return std::nullopt; }
        void save_match_title(const highlights::Title&) override {}
        std::filesystem::path title_font() override { return {}; }
        void save_title_font(const std::filesystem::path&) override {}
        std::vector<highlights::RallyKey> rally_order(const std::string&) override { return {}; }
        void save_rally_order(const std::string&,
                              const std::vector<highlights::RallyKey>&) override {}
        std::optional<highlights::StoredHighlights> saved;
    };
    class Rallies final : public highlights::RallySource {
      public:
        std::optional<highlights::RallyList> rallies(const std::string&) override {
            highlights::RallyList list{.rallies = {}, .fps = 59.94, .source = "detections"};
            for (int i = 0; i < 12; ++i) {
                list.rallies.push_back({.start_frame = i * 1000,
                                        .end_frame = i * 1000 + 100 + 37 * i,
                                        .game = false,
                                        .day = false});
            }
            return list;
        }
    };
    std::unique_ptr<highlights::HighlightStore> store(const std::filesystem::path&) override {
        return std::make_unique<Store>();
    }
    std::unique_ptr<highlights::RallySource> rallies(const std::filesystem::path&) override {
        return std::make_unique<Rallies>();
    }
    std::int64_t recorded_at(const std::filesystem::path& video) override {
        return static_cast<std::int64_t>(video.string().size()); // any fixed order
    }
};

/// Titles as solid blocks, without reading any font.
class BlockFonts final : public highlights::TitleFonts {
  public:
    std::vector<highlights::FontChoice> installed() override { return {{"/f/a.ttf", "Alpha"}}; }
    highlights::FontChoice fallback() override { return {"/f/default.ttf", "Default Bold"}; }
    highlights::TitleImage render(const std::filesystem::path&, const highlights::Title& title,
                                  int line_height, const std::filesystem::path&) override {
        highlights::TitleImage image;
        image.width = std::max(1, line_height * static_cast<int>(title.text.size()) / 2);
        image.height = line_height;
        image.rgba.assign(static_cast<std::size_t>(image.width * image.height) * 4, 255);
        return image;
    }
    std::vector<std::string> shapes(const std::filesystem::path&) override {
        return {"banner.svg"};
    }
};

class NoPlayback final : public highlights::PlaybackSource {
  public:
    std::unique_ptr<media::FrameDecoder> open(const std::filesystem::path& video,
                                              const std::filesystem::path&, int) override {
        throw media::MediaError("cannot open " + video.string());
    }
};

void use_offscreen_video_driver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "offscreen");
#else
    setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
#endif
}

} // namespace

TEST_CASE("the launcher window opens and renders the forms") {
    use_offscreen_video_driver();
    OneVideo scanner;
    IdleRunner runner;
    launcher::LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    const auto screenshot = std::filesystem::temp_directory_path() / "ttrally_launcher_smoke.bmp";
    std::filesystem::remove(screenshot);
    GreyPreview preview;
    gui::LauncherOptions options;
    options.max_frames = 10;
    options.screenshot = screenshot;
    gui::run_launcher(session, preview, options);
    CHECK(preview.requests >= 1);
    CHECK(std::filesystem::file_size(screenshot) > 0);
    std::filesystem::remove(screenshot);
}

TEST_CASE("the launcher shows the cut form of the selected video") {
    use_offscreen_video_driver();
    OneVideo scanner;
    IdleRunner runner;
    launcher::LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    session.select_video(0);
    // TTRALLY_SCREENSHOT_DIR keeps a screenshot of the window for looking at the layout.
    const char* keep = std::getenv("TTRALLY_SCREENSHOT_DIR");
    const auto screenshot = (keep != nullptr ? std::filesystem::path(keep)
                                             : std::filesystem::temp_directory_path()) /
                            "launcher_cut.bmp";
    GreyPreview preview;
    gui::LauncherOptions options;
    options.max_frames = 10;
    options.screenshot = screenshot;
    options.first_tab = "cut";
    gui::run_launcher(session, preview, options);
    CHECK(std::filesystem::file_size(screenshot) > 0);
    if (keep == nullptr) {
        std::filesystem::remove(screenshot);
    }
}

TEST_CASE("the launcher shows the highlights of the selected game") {
    use_offscreen_video_driver();
    OneVideo scanner;
    IdleRunner runner;
    launcher::LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    session.select_video(0);
    // TTRALLY_SCREENSHOT_DIR keeps a screenshot of the window for looking at the layout.
    const char* keep = std::getenv("TTRALLY_SCREENSHOT_DIR");
    const auto screenshot = (keep != nullptr ? std::filesystem::path(keep)
                                             : std::filesystem::temp_directory_path()) /
                            "launcher_highlights.bmp";
    GreyPreview preview;
    MemoryHighlights highlights;
    NoPlayback playback;
    gui::LauncherOptions options;
    options.max_frames = 10;
    options.screenshot = screenshot;
    options.first_tab = "highlights";
    options.highlights = &highlights;
    options.playback = &playback;
    BlockFonts fonts;
    options.fonts = &fonts;
    gui::run_launcher(session, preview, options);
    CHECK(std::filesystem::file_size(screenshot) > 0);
    if (keep == nullptr) {
        std::filesystem::remove(screenshot);
    }
}

TEST_CASE("the launcher shows a whole match day: its pipeline and highlights") {
    use_offscreen_video_driver();
    OneVideo scanner;
    scanner.extra_videos = 2;
    scanner.with_match = true;
    IdleRunner runner;
    launcher::LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    session.select_match_day("2026-10-03 Club");
    const char* keep = std::getenv("TTRALLY_SCREENSHOT_DIR");
    GreyPreview preview;
    MemoryHighlights highlights;
    NoPlayback playback;
    BlockFonts fonts;
    MemoryWhiteBalance white_balance;
    MemoryScenes scenes;
    for (const char* tab :
         {"matches", "pipeline", "white-balance", "highlights", "detect", "annotate", "cut"}) {
        const auto screenshot = (keep != nullptr ? std::filesystem::path(keep)
                                                 : std::filesystem::temp_directory_path()) /
                                std::format("launcher_day_{}.bmp", tab);
        gui::LauncherOptions options;
        options.max_frames = 10;
        options.screenshot = screenshot;
        options.first_tab = tab;
        options.highlights = &highlights;
        options.playback = &playback;
        options.fonts = &fonts;
        options.scenes = &scenes;
        options.white_balance = &white_balance;
        options.max_frames = 120; // the backgrounds are read in the background
        gui::run_launcher(session, preview, options);
        CHECK(std::filesystem::file_size(screenshot) > 0);
        if (keep == nullptr) {
            std::filesystem::remove(screenshot);
        }
    }
    CHECK(session.selected_match_day() == "2026-10-03 Club");
}
