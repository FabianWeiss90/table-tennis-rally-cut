// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/highlights_command.hpp"

#include "cli/resolution_option.hpp"
#include "cli/stream_progress_reporter.hpp"
#include "highlights/application/make_highlight_video.hpp"
#include "highlights/application/title_fonts.hpp"
#include "highlights/infrastructure/csv_highlight_store.hpp"
#include "highlights/infrastructure/ffmpeg_highlight_renderer.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/formatting.hpp"

#include <format>
#include <iostream>

namespace ttrally::cli {

namespace {

constexpr double kMinBitrateMbps = 1.0;
constexpr double kMaxBitrateMbps = 200.0;
constexpr double kMaxCrossfadeS = 5.0;

/// Name of the match day folder of the highlights (<match day>/ttrally-data/highlights).
std::string match_day_name(const std::filesystem::path& highlights_dir) {
    return std::filesystem::absolute(highlights_dir)
        .lexically_normal()
        .parent_path()
        .parent_path()
        .filename()
        .string();
}

/// "<match day>-highlights.mp4" next to the highlights.
std::filesystem::path match_day_video(const std::filesystem::path& highlights_dir) {
    const std::string name = match_day_name(highlights_dir);
    return highlights_dir / ((name.empty() ? std::string("match-day") : name) + "-highlights.mp4");
}

/// The library's shapes folder, if the match day lies in a library:
/// <library>/ttrally-data/title-shapes next to <library>/<match day>/ttrally-data/highlights.
std::filesystem::path library_shapes(const std::filesystem::path& highlights_dir) {
    const std::filesystem::path work = std::filesystem::absolute(highlights_dir)
                                           .lexically_normal()
                                           .parent_path(); // <match day>/ttrally-data
    return work.parent_path().parent_path() / work.filename() /
           std::string(highlights::kTitleShapesFolder);
}

/// A bare file name goes into the highlights directory, and a name without extension gets ".mp4".
std::filesystem::path output_video(std::filesystem::path out,
                                   const std::filesystem::path& highlights_dir) {
    if (!out.has_parent_path()) {
        out = highlights_dir / out;
    }
    if (!out.has_extension()) {
        out += ".mp4";
    }
    return out;
}

} // namespace

HighlightsCommand::HighlightsCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "highlights", "Make the highlight video of a game or of a match day from the rallies "
                        "marked in the launcher (ttrally gui, tab Highlights)")) {
    auto* game = command_->add_option("--video-id", video_id_,
                                      "Game: its game highlights, with its title over all clips");
    auto* day = command_->add_flag("--match-day", match_day_,
                                   "Match day: the match day highlights of all its games");
    game->excludes(day);
    command_->add_option("--highlights-dir", highlights_dir_,
                         "Marked rallies, one file per game (written by the launcher)")
        ->capture_default_str();
    command_->add_option("--out", out_,
                         "Output video; a bare name goes into --highlights-dir, \".mp4\" is added "
                         "if there is no extension (default: <highlights-dir>/<video-id>-<height>p"
                         ".mp4, for a match day <highlights-dir>/<match day>-highlights-<height>p"
                         ".mp4, e.g. -1080p)");
    command_->add_option("--before", before_s_, "Seconds kept before each rally")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    command_->add_option("--after", after_s_, "Seconds kept after each rally")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    command_->add_option("--crossfade", crossfade_s_,
                         "Seconds two clips fade into each other; 0: hard cuts")
        ->check(CLI::Range(0.0, kMaxCrossfadeS))
        ->capture_default_str();
    command_->add_flag("--rally-titles", rally_titles_,
                       "Match day: show each game's title over its clips");
    command_->add_option("--font", font_,
                         "TrueType font for the titles (default: a bold system font)")
        ->check(CLI::ExistingFile);
    command_->add_option("--shapes-dir", shapes_dir_,
                         "Folder of the SVG shapes for title backdrops (default: "
                         "<library>/ttrally-data/title-shapes, the library being the folder "
                         "around the match day)");
    command_->add_option("--codec", codec_,
                         "h265 (best quality per bit) or h264 (plays everywhere)")
        ->check(CLI::IsMember({"h265", "h264"}))
        ->capture_default_str();
    command_->add_option("--encoder", encoder_,
                         "Video encoder (default: the first of the codec that works: H.265 on the GPU, "
                         "H.264 with OpenH264 first)")
        ->capture_default_str();
    add_resolution_option(*command_, resolution_);
    command_->add_option("--bitrate", bitrate_mbps_,
                         "Video bit rate in Mbit/s, at least 1 (default: as cut)")
        ->check(CLI::Range(kMinBitrateMbps, kMaxBitrateMbps));
    command_->add_option("--cache-dir", cache_dir_, "Cache for frame timestamps")
        ->capture_default_str();
}

int HighlightsCommand::run() {
    if (video_id_.empty() && !match_day_) {
        throw CLI::ValidationError("give --video-id for a game or --match-day");
    }
    const bool default_name = out_.empty();
    if (default_name) { // the height is added once it is known
        out_ = match_day_ ? match_day_video(highlights_dir_)
                          : highlights_dir_ / (video_id_ + ".mp4");
    }
    out_ = output_video(out_, highlights_dir_);
    if (shapes_dir_.empty()) {
        shapes_dir_ = library_shapes(highlights_dir_);
    }
    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, cache_dir_);
    highlights::CsvHighlightStore store(highlights_dir_);
    highlights::FfmpegHighlightRenderer renderer;
    StreamProgressReporter progress(std::cout);
    highlights::MakeHighlightVideo make(probe, reader, store, renderer, progress);

    const auto result = make.execute(
        {.video_id = match_day_ ? std::nullopt : std::optional<std::string>(video_id_),
         .output = out_,
         .match_day_name = match_day_name(highlights_dir_),
         .margins = {before_s_, after_s_},
         .rally_titles = rally_titles_,
         .render = {.crossfade_s = crossfade_s_,
                    .encoding = {.mode = cutting::CutMode::Exact,
                                 .codec = codec_ == "h264" ? cutting::VideoCodec::H264
                                                           : cutting::VideoCodec::H265,
                                 .encoder = encoder_,
                                 .bitrate_mbps = bitrate_mbps_,
                                 .max_height = max_height_of(resolution_)},
                    .font = font_,
                    .shapes = shapes_dir_},
         .resolution_in_name = default_name});
    std::cout << std::format("Done in {}: {} rallies from {} game(s) in {} clips, {} of "
                             "video (encoded with {} at {:.1f} Mbit/s).\n",
                             io::format_elapsed(result.seconds), result.rallies,
                             result.games.size(), result.clips,
                             io::format_clock(result.render.duration_s),
                             result.render.video_encoder, result.render.bitrate_mbps);
    std::cout << "Highlight video: " << result.output.string() << '\n';
    return 0;
}

} // namespace ttrally::cli
