// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The FFmpeg video joiner on clips generated with the ffmpeg command-line tool (skipped if it is
// not installed): parts follow each other without gaps, nothing is replaced, and parts that
// would need re-encoding are refused.

#include "cutting/infrastructure/ffmpeg_video_joiner.hpp"
#include "media/application/media_error.hpp"
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

/// A clip of `seconds` at 60 fps with a tone, keyframe every second.
bool make_clip(const std::filesystem::path& path, const std::string& size, int seconds) {
    return run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size={}:rate=60 "
                           "-f lavfi -i sine=frequency=440:sample_rate=48000 -t {} "
                           "-c:v mpeg4 -q:v 4 -g 60 -c:a aac \"{}\"",
                           size, seconds, path.string()));
}

} // namespace

TEST_CASE("the video joiner appends the parts without re-encoding") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_merge_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    REQUIRE(make_clip(dir / "part1.mp4", "320x180", 2));
    REQUIRE(make_clip(dir / "part2.mp4", "320x180", 3));
    REQUIRE(make_clip(dir / "other.mp4", "160x90", 1));
    FfmpegVideoJoiner joiner;
    SilentProgressReporter progress;
    const std::vector<std::filesystem::path> parts{dir / "part1.mp4", dir / "part2.mp4"};

    SECTION("one after another") {
        const auto out = dir / "merged" / "game.mp4";
        CHECK(joiner.join(parts, out, progress) == Catch::Approx(5.0).margin(0.03));
        media::FfmpegMediaProbe probe;
        const auto info = probe.probe(out);
        REQUIRE(info.video);
        REQUIRE(info.audio);
        CHECK(info.video->codec == "mpeg4"); // copied, not re-encoded
        media::FfmpegMediaReader reader;
        const auto content = reader.read(
            out, {.audio_sample_rate = 8000, .video_stream_index = info.video->index});
        CHECK(content.video_timestamps->frame_count() == 300);
        CHECK(static_cast<double>(content.audio->samples.size()) / 8000.0 ==
              Catch::Approx(5.0).margin(0.05));
        CHECK_FALSE(std::filesystem::exists(dir / "merged" / "game.merging.mp4"));
    }
    SECTION("an existing file is never replaced") {
        const auto out = dir / "taken.mp4";
        std::filesystem::copy_file(dir / "other.mp4", out);
        const auto size = std::filesystem::file_size(out);
        CHECK_THROWS_AS(joiner.join(parts, out, progress), media::MediaError);
        CHECK(std::filesystem::file_size(out) == size);
    }
    SECTION("parts that would need re-encoding are refused, nothing is left behind") {
        const std::vector<std::filesystem::path> mixed{dir / "part1.mp4", dir / "other.mp4"};
        CHECK_THROWS_AS(joiner.join(mixed, dir / "mixed.mp4", progress), media::MediaError);
        CHECK_FALSE(std::filesystem::exists(dir / "mixed.mp4"));
        CHECK_FALSE(std::filesystem::exists(dir / "mixed.merging.mp4"));
    }
    std::filesystem::remove_all(dir);
}
