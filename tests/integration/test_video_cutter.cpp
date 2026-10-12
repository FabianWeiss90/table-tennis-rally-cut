// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The FFmpeg video cutter on a clip generated with the ffmpeg command-line tool (skipped if it is
// not installed): exact cuts contain exactly the frames of the sections, fast cuts start at
// keyframes, and both keep the audio.

#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>

using namespace ttrally;
using namespace ttrally::cutting;

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

struct CutClip {
    std::int64_t frames = 0;
    double audio_s = 0.0;
};

CutClip inspect(const std::filesystem::path& path) {
    media::FfmpegMediaProbe probe;
    const auto info = probe.probe(path);
    REQUIRE(info.video);
    REQUIRE(info.audio);
    media::FfmpegMediaReader reader;
    const auto content =
        reader.read(path, {.audio_sample_rate = 8000, .video_stream_index = info.video->index});
    return {content.video_timestamps->frame_count(),
            static_cast<double>(content.audio->samples.size()) / 8000.0};
}

} // namespace

TEST_CASE("the video cutter writes exactly the frames of the sections") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_cut_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    const auto clip = dir / "clip.mp4";
    // 8 s at 60 fps with a keyframe every second, plus a tone
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate=60 "
                            "-f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 "
                            "-c:v mpeg4 -q:v 4 -g 60 -c:a aac \"{}\"",
                            clip.string())));
    const std::vector<CutSection> sections{{1.5, 2.5, 1}, {4.0, 5.25, 1}};
    FfmpegVideoCutter cutter;
    SilentProgressReporter progress;

    SECTION("exact") {
        const auto result = cutter.cut(clip, sections, dir / "exact.mp4",
                                       {.mode = CutMode::Exact, .encoder = "auto"}, progress);
        CHECK(result.duration_s == Catch::Approx(2.25));
        CHECK_FALSE(result.video_encoder.empty());
        const auto cut = inspect(dir / "exact.mp4");
        CHECK(cut.frames == 135); // 60 + 75
        CHECK(cut.audio_s == Catch::Approx(2.25).margin(0.05));
    }
    SECTION("fast") {
        const auto result =
            cutter.cut(clip, sections, dir / "fast.mp4", {.mode = CutMode::Fast}, progress);
        CHECK(result.video_encoder == "copy");
        // Widened to the keyframes at 1, 3, 4 and 6 s: 1-3 s and 4-6 s
        CHECK(result.duration_s == Catch::Approx(4.0).margin(0.05));
        const auto cut = inspect(dir / "fast.mp4");
        CHECK(cut.frames == 240);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("unknown encoders are refused") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_cut_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    const auto clip = dir / "clip.mp4";
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=160x90:rate=30 "
                            "-t 2 -c:v mpeg4 \"{}\"",
                            clip.string())));
    FfmpegVideoCutter cutter;
    SilentProgressReporter progress;
    const std::vector<CutSection> sections{{0.5, 1.0, 1}};
    CHECK_THROWS(cutter.cut(clip, sections, dir / "out.mp4",
                            {.mode = CutMode::Exact, .encoder = "no_such_encoder"}, progress));
    CHECK_FALSE(automatic_video_encoders(VideoCodec::H264).empty());
    // H.265 falls back to H.264 encoders where no H.265 encoder exists
    CHECK(automatic_video_encoders(VideoCodec::H265).size() >=
          automatic_video_encoders(VideoCodec::H264).size());
    std::filesystem::remove_all(dir);
}
