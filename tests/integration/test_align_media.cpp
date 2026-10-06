// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// End-to-end test of `align` with the real adapters (FFmpeg, pocketfft, CSV/HTML writers). The
// clips are generated at runtime with the ffmpeg command-line tool; the test is skipped if it is
// not installed.

#include "alignment/application/align_videos.hpp"
#include "alignment/infrastructure/csv_alignment_writer.hpp"
#include "alignment/infrastructure/html_report_writer.hpp"
#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_sampler.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/csv.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>
#include <string>
#include <vector>

namespace {

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

bool ffmpeg_available() { return run("ffmpeg -hide_banner -version"); }

std::string quoted(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }

struct Piece {
    double start;
    double end;
};

const std::vector<Piece> kPieces{{5.0, 11.5}, {17.0, 23.25}, {30.0, 36.0}};

std::filesystem::path make_temp_dir() {
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_media_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("align end-to-end with generated clips") {
    if (!ffmpeg_available()) {
        SKIP("ffmpeg command-line tool not found");
    }
    ttrally::media::configure_ffmpeg_logging(false);

    for (const char* rate : {"60", "60000/1001"}) {
        DYNAMIC_SECTION("original at " << rate << " fps") {
            const auto dir = make_temp_dir();
            const auto original = dir / "original.mp4";
            const auto cut = dir / "cut.mp4";

            REQUIRE(run(std::format(
                "ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate={} "
                "-f lavfi -i anoisesrc=color=pink:sample_rate=48000:amplitude=0.3:seed=7 -t 40 "
                "-c:v mpeg4 -q:v 5 -c:a aac -b:a 128k {}",
                rate, quoted(original))));

            std::string filter;
            std::string inputs;
            for (std::size_t i = 0; i < kPieces.size(); ++i) {
                filter += std::format("[0:v]trim=start={0}:end={1},setpts=PTS-STARTPTS[v{2}];"
                                      "[0:a]atrim=start={0}:end={1},asetpts=PTS-STARTPTS[a{2}];",
                                      kPieces[i].start, kPieces[i].end, i);
                inputs += std::format("[v{0}][a{0}]", i);
            }
            filter += std::format("{}concat=n={}:v=1:a=1[vc][ac];[vc]scale=160:90,fps=30[v];"
                                  "[ac]aresample=44100[a]",
                                  inputs, kPieces.size());
            REQUIRE(run(std::format("ffmpeg -hide_banner -y -i {} -filter_complex \"{}\" "
                                    "-map \"[v]\" -map \"[a]\" -c:v mpeg4 -q:v 5 -c:a aac "
                                    "-b:a 128k {}",
                                    quoted(original), filter, quoted(cut))));

            ttrally::media::FfmpegMediaProbe probe;
            ttrally::media::FfmpegMediaReader ffmpeg_reader;
            ttrally::media::CachingMediaReader reader(ffmpeg_reader, dir / "cache");
            ttrally::media::FfmpegFrameSamplerFactory samplers;
            const ttrally::alignment::PocketFftSignalMatcher matcher;
            ttrally::SilentProgressReporter progress;
            ttrally::alignment::AlignVideos align(probe, reader, samplers, matcher, progress);

            ttrally::alignment::AlignVideosRequest request;
            request.original = original;
            request.cut = cut;
            request.decode_backend = ttrally::media::DecodeBackend::Cpu;
            request.settings.local_search_s = 10.0;

            const auto report = align.execute(request);
            INFO([&] {
                std::string text;
                for (const auto& warning : report.warnings) {
                    text += warning + "\n";
                }
                return text;
            }());
            REQUIRE(report.candidates.size() == kPieces.size());
            CHECK(report.original_constant_frame_rate);
            CHECK(report.cut_decode_backend == ttrally::media::DecodeBackend::Cpu);

            const auto segments_csv = dir / "out" / "clip.csv";
            ttrally::alignment::write_segments_csv(segments_csv, report);
            ttrally::alignment::write_gaps_csv(ttrally::alignment::gaps_path_for(segments_csv),
                                               report);
            ttrally::alignment::write_html_report(dir / "out" / "clip.html", report);
            CHECK(std::filesystem::exists(dir / "out" / "clip.gaps.csv"));
            CHECK(std::filesystem::exists(dir / "out" / "clip.html"));

            const auto rows = ttrally::io::read_csv(segments_csv);
            REQUIRE(rows.size() == kPieces.size() + 1);
            const double fps = std::string(rate) == "60" ? 60.0 : 60000.0 / 1001.0;
            for (std::size_t k = 0; k < kPieces.size(); ++k) {
                INFO("segment " << k + 1);
                const auto& row = rows[k + 1];
                CHECK(std::abs(std::stod(row[3]) - kPieces[k].start) < 0.02);
                CHECK(std::abs(std::stod(row[4]) - kPieces[k].end) < 0.02);
                CHECK(std::llabs(std::stoll(row[5]) - std::llround(kPieces[k].start * fps)) <= 1);
            }

            // A second run is served from the cache and gives the same result
            const auto again = align.execute(request);
            REQUIRE(again.candidates.size() == report.candidates.size());
            CHECK(again.candidates.front().orig_start_s == report.candidates.front().orig_start_s);

            std::filesystem::remove_all(dir);
        }
    }
}
