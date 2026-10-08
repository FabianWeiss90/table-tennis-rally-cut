// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Smoke test of the annotation GUI: opens the window with SDL's invisible "offscreen" video
// driver and renders some frames of a clip generated with the ffmpeg command-line tool
// (skipped if it is not installed).

#include "annotation/application/annotation_session.hpp"
#include "gui/annotator_app.hpp"
#include "media/application/frame_prefetcher.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>

using namespace ttrally;

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

void use_offscreen_video_driver() {
#ifdef _WIN32
    _putenv_s("SDL_VIDEO_DRIVER", "offscreen");
#else
    setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
#endif
}

class NoAnnotations final : public annotation::AnnotationRepository {
  public:
    annotation::StoredLabels load(const std::string&) override { return {}; }
    void save(const annotation::AnnotationSheet&) override {}
};

class OneCandidate final : public annotation::ReviewItemSource {
  public:
    std::vector<annotation::ReviewItem> load() override {
        return {{annotation::ReviewKind::Gap, 1, 0, 59},
                {annotation::ReviewKind::Candidate, 1, 60, 150}};
    }
};

class NoStates final : public annotation::ReviewStateStore {
  public:
    annotation::ReviewStatusMap load(const std::string&) override { return {}; }
    void save(const std::string&, const std::vector<annotation::ReviewItem>&) override {}
};

} // namespace

TEST_CASE("the annotation window opens and renders frames") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    use_offscreen_video_driver();

    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_gui_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    const auto clip = dir / "clip.mp4";
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=640x360:rate=60 "
                            "-t 4 -c:v mpeg4 -q:v 4 \"{}\"",
                            clip.string())));

    media::FfmpegMediaReader reader;
    const auto timestamps =
        *reader.read(clip, {.audio_sample_rate = std::nullopt, .video_stream_index = 0})
             .video_timestamps;
    NoAnnotations annotations;
    OneCandidate items;
    NoStates states;
    annotation::AnnotationSession session({"clip", {60, 1}, timestamps.frame_count()},
                                          annotations, items, states);
    media::FfmpegFrameDecoderFactory decoders;
    media::FramePrefetcher frames(
        decoders.open(clip, timestamps, media::DecodeBackend::Cpu, {.height = 360}), 64);

    // TTRALLY_SCREENSHOT_DIR keeps a screenshot of the window for looking at the layout.
    const char* keep = std::getenv("TTRALLY_SCREENSHOT_DIR");
    const auto screenshot =
        (keep != nullptr ? std::filesystem::path(keep) : dir) / "annotator.bmp";
    const gui::AnnotatorOptions options{.title = "test", .max_frames = 30,
                                        .screenshot = screenshot};
    REQUIRE_NOTHROW(gui::run_annotator(session, frames, {60, 1}, options));
    CHECK(std::filesystem::file_size(screenshot) > 0);
    frames.wait_until_idle();
    CHECK_FALSE(frames.error());
    CHECK(frames.frame(0)); // the first item (gap 1) starts at frame 0
    std::filesystem::remove_all(dir);
}
