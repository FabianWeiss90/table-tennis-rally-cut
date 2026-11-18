// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Smoke test of the launcher window: opens it with SDL's invisible "offscreen" video driver,
// renders a few frames with in-memory workspace and runner and saves a screenshot.

#include "gui/launcher_app.hpp"
#include "media/application/media_error.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
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
        workspace.videos = {video};
        workspace.trainable = {video.key(), "2026-10-10 Away/other"};
        return workspace;
    }
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
    gui::run_launcher(session, preview, options);
    CHECK(std::filesystem::file_size(screenshot) > 0);
    if (keep == nullptr) {
        std::filesystem::remove(screenshot);
    }
}
