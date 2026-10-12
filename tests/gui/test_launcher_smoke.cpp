// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Smoke test of the launcher window: opens it with SDL's invisible "offscreen" video driver,
// renders a few frames with in-memory workspace and runner and saves a screenshot.

#include "gui/launcher_app.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <utility>

using namespace ttrally;

namespace {

class OneVideo final : public launcher::WorkspaceScanner {
  public:
    launcher::WorkspaceSnapshot scan() override {
        launcher::WorkspaceSnapshot workspace;
        launcher::VideoEntry video;
        video.original = "data/Match_FHD.mp4";
        video.video_id = "match";
        video.cut_candidates = {"data/Match_FHD_cut.mp4"};
        video.aligned = true;
        video.annotation = launcher::AnnotationProgress::InProgress;
        workspace.videos = {video};
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
    int requests = 0;
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
    launcher::LauncherSession session(scanner, runner, "ttrally", ".");
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
