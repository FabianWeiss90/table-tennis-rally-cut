// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The MP4 header repair on H.265 clips generated with the ffmpeg command-line tool (skipped if
// it or an H.265 encoder is missing), and the header of videos cut by ttrally.

#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"
#include "cutting/infrastructure/mp4_repair.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <string>

using namespace ttrally;
using namespace ttrally::cutting;

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#define popen _popen
#define pclose _pclose
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

/// What a command prints (stdout).
std::string output_of(const std::string& command) {
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    REQUIRE(pipe);
    std::string text;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        text += buffer.data();
    }
    return text;
}

/// MD5 of the decoded pictures, and the number of decoder complaints.
std::string decoded_md5(const std::filesystem::path& file) {
    return output_of(std::format("ffmpeg -v error -i \"{}\" -map 0:v -f md5 - 2>&1",
                                 file.string()));
}

std::filesystem::path temp_dir() {
    std::random_device device;
    auto dir = std::filesystem::temp_directory_path() /
               std::format("ttrally_repair_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("an H.265 file tagged hev1 is repaired without changing its pictures") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    const auto dir = temp_dir();
    const auto clip = dir / "hev1.mp4";
    // Small pictures: NAL sizes of 256-511 bytes begin with 00 00 01, like a start code
    if (!run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate=30 "
                         "-f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 -c:v libx265 "
                         "-x265-params log-level=none:repeat-headers=1 -g 30 -tag:v hev1 "
                         "-c:a aac \"{}\"",
                         clip.string()))) {
        std::filesystem::remove_all(dir);
        SKIP("no libx265 in the ffmpeg command-line tool");
    }
    const Mp4Check before = check_mp4(clip);
    CHECK(before.h26x);
    CHECK_FALSE(before.tagged_hvc1);
    CHECK_FALSE(before.fine());
    const std::string pictures = decoded_md5(clip);
    REQUIRE(pictures.starts_with("MD5="));

    CHECK(repair_mp4(clip) == RepairOutcome::Repaired);
    const Mp4Check after = check_mp4(clip);
    CHECK(after.tagged_hvc1);
    CHECK(after.fine());
    CHECK(after.header_sets == before.stream_sets);
    CHECK(decoded_md5(clip) == pictures); // lossless, and no decoder complaints
    CHECK(repair_mp4(clip) == RepairOutcome::AlreadyFine);
    CHECK_FALSE(std::filesystem::exists(dir / "hev1.mp4.repairing.mp4"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("cut videos carry the stream's parameter sets in their header") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    const auto dir = temp_dir();
    const auto clip = dir / "clip.mp4";
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate=60 "
                            "-f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 "
                            "-c:v mpeg4 -q:v 4 -g 60 -c:a aac \"{}\"",
                            clip.string())));
    FfmpegVideoCutter cutter;
    SilentProgressReporter progress;
    for (const VideoCodec codec : {VideoCodec::H265, VideoCodec::H264}) {
        const auto out = dir / (codec == VideoCodec::H265 ? "h265.mp4" : "h264.mp4");
        const CutResult result = cutter.cut(clip, std::vector<CutSection>{{0.5, 2.0, 1}}, out,
                                            {.mode = CutMode::Exact, .codec = codec}, progress);
        const Mp4Check check = check_mp4(out);
        INFO(result.video_encoder);
        if (check.h26x) { // mpeg4 is the last fallback and has no parameter sets
            CHECK(check.fine());
        }
    }
    std::filesystem::remove_all(dir);
}
