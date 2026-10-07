// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Frame accuracy of the FFmpeg frame decoder: a frame decoded by random access (after a seek)
// must be bit-identical to the same frame decoded sequentially from the start. The clip is
// generated with the ffmpeg command-line tool; the test is skipped if it is not installed.

#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <vector>

using namespace ttrally::media;

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

} // namespace

TEST_CASE("random access delivers exactly the requested frame") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    configure_ffmpeg_logging(false);
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_frame_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    const auto clip = dir / "clip.mp4";
    // Long GOP (keyframe every 50 frames) and B-frames, like camera footage
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi "
                            "-i testsrc2=size=320x180:rate=60000/1001 -t 6 "
                            "-c:v mpeg4 -q:v 4 -g 50 -bf 2 \"{}\"",
                            clip.string())));

    FfmpegMediaReader reader;
    const auto content = reader.read(clip, {.audio_sample_rate = std::nullopt,
                                            .video_stream_index = 0});
    const VideoTimestamps& timestamps = *content.video_timestamps;
    REQUIRE(timestamps.frame_count() > 300);

    FfmpegFrameDecoderFactory factory;
    auto sequential = factory.open(clip, timestamps, DecodeBackend::Cpu, {.height = 180});
    std::map<std::int64_t, std::vector<std::uint8_t>> reference;
    sequential->decode(0, timestamps.frame_count() - 1, [&](VideoFrame&& frame) {
        reference[frame.index] = std::move(frame.planes);
        return true;
    });
    REQUIRE(static_cast<std::int64_t>(reference.size()) == timestamps.frame_count());

    auto random_access = factory.open(clip, timestamps, DecodeBackend::Cpu, {.height = 180});
    CHECK(random_access->frame_size().height == 180);
    for (const std::int64_t index : {200, 49, 50, 51, 199, 0, 300, 125, 124, 340}) {
        INFO("frame " << index);
        std::optional<VideoFrame> delivered;
        random_access->decode(index, index, [&](VideoFrame&& frame) {
            delivered = std::move(frame);
            return true;
        });
        REQUIRE(delivered);
        CHECK(delivered->index == index);
        CHECK(delivered->planes == reference.at(index));
    }

    SECTION("selected frames match the sequential ones") {
        auto selective = factory.open(clip, timestamps, DecodeBackend::Cpu, {.height = 180});
        const std::vector<std::int64_t> wanted{0, 6, 12, 49, 50, 51, 120, 300, 301, 359};
        std::vector<std::int64_t> delivered;
        selective->decode_selected(wanted, [&](VideoFrame&& frame) {
            CHECK(frame.planes == reference.at(frame.index));
            delivered.push_back(frame.index);
            return true;
        });
        CHECK(delivered == wanted);
    }
    SECTION("RGB output with an exact size") {
        auto rgb = factory.open(clip, timestamps, DecodeBackend::Cpu,
                                {.height = 224, .width = 392, .layout = PixelLayout::Rgb24});
        CHECK(rgb->frame_size().width == 392);
        std::optional<VideoFrame> frame;
        rgb->decode_selected(std::vector<std::int64_t>{42}, [&](VideoFrame&& f) {
            frame = std::move(f);
            return true;
        });
        REQUIRE(frame);
        CHECK(frame->index == 42);
        CHECK(frame->layout == PixelLayout::Rgb24);
        CHECK(frame->planes.size() == static_cast<std::size_t>(392 * 224 * 3));
    }
    std::filesystem::remove_all(dir);
}
