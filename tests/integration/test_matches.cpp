// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Matches recorded in several files on clips generated with the ffmpeg command-line tool
// (skipped if it is not installed): the part list is read as one continuous video, the scanner
// lists the match as one video with its parts' pre-cuts, and cuts across a part boundary are
// exact.

#include "cutting/infrastructure/csv_white_balance_store.hpp"
#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"
#include "launcher/infrastructure/csv_match_store.hpp"
#include "launcher/infrastructure/library_scanner.hpp"
#include "media/infrastructure/part_list.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <map>
#include <optional>
#include <format>
#include <random>

using namespace ttrally;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

/// `seconds` at 60 fps with a tone; B-frames and a keyframe every 50 frames, like camera footage.
bool make_clip(const fs::path& path, int seconds) {
    fs::create_directories(path.parent_path());
    return run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=160x90:rate=60 "
                           "-f lavfi -i sine=frequency=440:sample_rate=48000 -t {} "
                           "-c:v mpeg4 -q:v 4 -g 50 -bf 2 -c:a aac \"{}\"",
                           seconds, path.string()));
}

} // namespace

TEST_CASE("a match of several files is one video with one continuous timeline") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const fs::path library = fs::temp_directory_path() /
                             std::format("ttrally_matches_test_{:08x}", device());
    const fs::path day = library / "2026-11-21-Club";
    const fs::path work = day / "ttrally-data";
    REQUIRE(make_clip(day / "Weiss-Becker-1.mp4", 2));
    REQUIRE(make_clip(day / "Weiss-Becker-2.mp4", 3));
    REQUIRE(make_clip(day / "Single.mp4", 1));
    REQUIRE(make_clip(day / "Liimba" / "7_Weiss-Becker-2.mp4", 1));

    launcher::CsvMatchStore store;
    store.save(work, {{"weiss-becker", {"Weiss-Becker-1.mp4", "Weiss-Becker-2.mp4"}}});
    const fs::path list = work / "matches" / "weiss-becker.ffconcat";
    REQUIRE(fs::exists(list));
    CHECK(media::read_part_list(list) ==
          std::vector<fs::path>{(day / "Weiss-Becker-1.mp4").lexically_normal(),
                                (day / "Weiss-Becker-2.mp4").lexically_normal()});
    CHECK(store.load(work) ==
          std::vector<launcher::Match>{
              {"weiss-becker", {"Weiss-Becker-1.mp4", "Weiss-Becker-2.mp4"}}});

    SECTION("read as one video") {
        media::FfmpegMediaProbe probe;
        const auto info = probe.probe(list);
        REQUIRE(info.video);
        media::FfmpegMediaReader reader;
        const auto content =
            reader.read(list, {.audio_sample_rate = 8000, .video_stream_index = info.video->index});
        CHECK(content.video_timestamps->frame_count() == 300);
        CHECK(static_cast<double>(content.audio->samples.size()) / 8000.0 ==
              Catch::Approx(5.0).margin(0.05));
    }
    SECTION("random access across the parts delivers exactly the requested frame") {
        media::FfmpegMediaReader reader;
        const auto timestamps = *reader.read(list, {.audio_sample_rate = std::nullopt,
                                                    .video_stream_index = 0})
                                     .video_timestamps;
        media::FfmpegFrameDecoderFactory factory;
        auto sequential = factory.open(list, timestamps, media::DecodeBackend::Cpu, {.height = 90});
        std::map<std::int64_t, std::vector<std::uint8_t>> reference;
        sequential->decode(0, timestamps.frame_count() - 1, [&](media::VideoFrame&& frame) {
            reference[frame.index] = std::move(frame.planes);
            return true;
        });
        REQUIRE(reference.size() == 300);
        auto random_access =
            factory.open(list, timestamps, media::DecodeBackend::Cpu, {.height = 90});
        for (const std::int64_t index : {250, 119, 120, 121, 10, 299, 170, 60}) {
            INFO("frame " << index);
            std::optional<media::VideoFrame> delivered;
            random_access->decode(index, index, [&](media::VideoFrame&& frame) {
                delivered = std::move(frame);
                return true;
            });
            REQUIRE(delivered);
            CHECK(delivered->index == index);
            CHECK(delivered->planes == reference.at(index));
        }
    }
    SECTION("listed by the scanner as one video with its parts and their pre-cuts") {
        const launcher::WorkspaceSnapshot workspace =
            launcher::LibraryScanner(library).scan(library);
        REQUIRE(workspace.videos.size() == 2);
        const auto match = std::ranges::find(workspace.videos, "weiss-becker",
                                             &launcher::VideoEntry::video_id);
        REQUIRE(match != workspace.videos.end());
        CHECK(match->original == list.string());
        CHECK(match->match_day == "2026-11-21-Club");
        CHECK(match->parts.size() == 2);
        REQUIRE(match->cut_candidates.size() == 1);
        const fs::path pre_cuts = match->cut_candidates.front();
        CHECK(pre_cuts == work / "matches" / "weiss-becker.pre-cut.ffconcat");
        CHECK(media::read_part_list(pre_cuts) ==
              std::vector<fs::path>{(day / "Liimba" / "7_Weiss-Becker-2.mp4").lexically_normal()});
        CHECK(workspace.videos[0].video_id == "single"); // sorted by id; the parts are hidden
    }
    SECTION("cut exactly across the boundary of two parts") {
        cutting::FfmpegVideoCutter cutter;
        SilentProgressReporter progress;
        const std::vector<cutting::CutSection> sections{{1.5, 2.5, 1}};
        static_cast<void>(cutter.cut(list, sections, library / "cut.mp4",
                                     {.mode = cutting::CutMode::Exact, .encoder = "auto"},
                                     progress));
        media::FfmpegMediaProbe probe;
        const auto info = probe.probe(library / "cut.mp4");
        REQUIRE(info.video);
        media::FfmpegMediaReader reader;
        CHECK(reader.read(library / "cut.mp4", {.audio_sample_rate = std::nullopt,
                                                .video_stream_index = info.video->index})
                  .video_timestamps->frame_count() == 60);
    }
    SECTION("split again: the list is removed") {
        store.save(work, {});
        CHECK_FALSE(fs::exists(list));
        CHECK(store.load(work).empty());
    }
    fs::remove_all(library);
}

TEST_CASE("the white balance chosen for a part corrects only that part's frames") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const fs::path day = fs::temp_directory_path() /
                         std::format("ttrally_white_balance_test_{:08x}", device());
    const fs::path work = day / "ttrally-data";
    for (const char* part : {"Warm-1.mp4", "Warm-2.mp4"}) {
        fs::create_directories(day);
        REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi "
                                "-i color=c=0xC8B496:size=160x90:rate=60 -f lavfi "
                                "-i sine=sample_rate=48000 -t 2 -c:v mpeg4 -q:v 2 -g 30 "
                                "-c:a aac \"{}\"",
                                (day / part).string())));
    }
    launcher::CsvMatchStore().save(work, {{"warm", {"Warm-1.mp4", "Warm-2.mp4"}}});
    const fs::path list = work / "matches" / "warm.ffconcat";
    const cutting::RgbGains neutral = cutting::neutral_gains({200, 180, 150});
    const cutting::WhiteBalanceSettings settings{
        .files = {{"Warm-1.mp4", cutting::RelativeRect{0.1, 0.1, 0.9, 0.9}, neutral},
                  {"Warm-2.mp4", std::nullopt, {}}},
        .reference = "Warm-1.mp4",
        .temperature = -0.25,
        .tint = 0.125,
        .brightness = -0.5,
        .match_brightness = true};
    cutting::CsvWhiteBalanceStore().save(work, settings);
    const cutting::WhiteBalanceSettings read = cutting::CsvWhiteBalanceStore().load(work);
    CHECK(read.files[0].file == "Warm-1.mp4");
    REQUIRE(read.files[0].area);
    CHECK(read.files[0].area->right == Catch::Approx(0.9));
    CHECK_FALSE(read.files[1].area);
    CHECK(read.files[0].gains.red == Catch::Approx(neutral.red).margin(1e-6));
    CHECK(read.reference == "Warm-1.mp4");
    CHECK(read.temperature == Catch::Approx(-0.25));
    CHECK(read.tint == Catch::Approx(0.125));
    CHECK(read.brightness == Catch::Approx(-0.5));
    CHECK(read.match_brightness);
    const auto spans = cutting::color_spans(list, work / "white-balance.csv");
    REQUIRE(spans.size() == 1); // the unchanged part needs none
    CHECK(spans[0].start_s == 0.0);
    CHECK(spans[0].end_s == Catch::Approx(2.0).margin(0.05));

    cutting::FfmpegVideoCutter cutter;
    SilentProgressReporter progress;
    const std::vector<cutting::CutSection> sections{{1.5, 2.5, 1}};
    static_cast<void>(cutter.cut(list, sections, day / "cut.mp4",
                                 {.mode = cutting::CutMode::Exact,
                                  .encoder = "auto",
                                  .white_balance = work / "white-balance.csv"},
                                 progress));
    media::FfmpegMediaReader reader;
    const auto timestamps = *reader.read(day / "cut.mp4", {.audio_sample_rate = std::nullopt,
                                                           .video_stream_index = 0})
                                 .video_timestamps;
    media::FfmpegFrameDecoderFactory factory;
    auto decoder =
        factory.open(day / "cut.mp4", timestamps, media::DecodeBackend::Cpu, {.height = 90});
    // Mean chroma distance from neutral of a frame
    const auto tint = [&](std::int64_t index) {
        double distance = 0;
        decoder->decode(index, index, [&](media::VideoFrame&& frame) {
            const auto u = frame.u();
            const auto v = frame.v();
            for (std::size_t i = 0; i < u.size(); ++i) {
                distance += std::abs(u[i] - 128.0) + std::abs(v[i] - 128.0);
            }
            distance /= static_cast<double>(u.size());
            return true;
        });
        return distance;
    };
    CHECK(tint(10) < 3.0);  // part 1: corrected to grey
    CHECK(tint(50) > 15.0); // part 2: as recorded
    fs::remove_all(day);
}
