// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/cut_command.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "cli/resolution_option.hpp"
#include "cli/stream_progress_reporter.hpp"
#include "cutting/application/cut_video.hpp"
#include "cutting/infrastructure/cut_list_choice.hpp"
#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/formatting.hpp"

#include <format>
#include <iostream>
#include <stdexcept>

namespace ttrally::cli {

namespace {

constexpr double kMinBitrateMbps = 1.0;
constexpr double kMaxBitrateMbps = 200.0;

} // namespace

CutCommand::CutCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "cut", "Cut a video down to its rallies, with a margin before and after each")) {
    command_->add_option("VIDEO", video_, "Original video")->required()->check(CLI::ExistingFile);
    command_->add_option("--video-id", video_id_,
                         "Video id (default: file name without extension)");
    command_->add_option("--rallies", rallies_,
                         "Cut list in the label format (default: the annotations "
                         "<annotations-dir>/<video-id>.csv if they are complete, else the "
                         "refined detections <refined-dir>/<video-id>.csv, else "
                         "<detections-dir>/<video-id>.csv)")
        ->check(CLI::ExistingFile);
    command_->add_option("--annotations-dir", annotations_dir_,
                         "Where the default looks for annotations")
        ->capture_default_str();
    command_->add_option("--refined-dir", refined_dir_,
                         "Where the default looks for refined detections (annotate --detections)")
        ->capture_default_str();
    command_->add_option("--detections-dir", detections_dir_,
                         "Where the default looks for detections")
        ->capture_default_str();
    command_->add_option("--out", out_,
                         "Output video, used as given (default: <out-dir>/<video-id>-<height>p.mp4, "
                         "e.g. game-1080p.mp4)");
    command_->add_option("--out-dir", out_dir_, "Folder of the default output video")
        ->capture_default_str();
    command_->add_option("--before", before_s_, "Seconds kept before each rally")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    command_->add_option("--after", after_s_, "Seconds kept after each rally")
        ->check(CLI::NonNegativeNumber)
        ->capture_default_str();
    command_->add_flag("--fast", fast_,
                       "Copy without re-encoding (seconds instead of minutes, but each cut is "
                       "widened to the nearest keyframes)");
    command_->add_option("--codec", codec_,
                         "h265 (best quality per bit) or h264 (plays everywhere)")
        ->check(CLI::IsMember({"h265", "h264"}))
        ->capture_default_str();
    command_->add_option("--encoder", encoder_,
                         "Video encoder, e.g. hevc_vaapi, h264_nvenc, libopenh264 (default: the "
                         "first of the codec that works: H.265 on the GPU, H.264 with OpenH264 first)")
        ->capture_default_str();
    add_resolution_option(*command_, resolution_);
    command_->add_option("--bitrate", bitrate_mbps_,
                         "Video bit rate in Mbit/s, at least 1 (default: by resolution and frame rate; for "
                         "Full HD at 60 fps 4 with H.265, 8 with H.264)")
        ->check(CLI::Range(kMinBitrateMbps, kMaxBitrateMbps));
    command_->add_option("--cache-dir", cache_dir_, "Cache for frame timestamps")
        ->capture_default_str();
}

int CutCommand::run() {
    if (video_id_.empty()) {
        video_id_ = video_.stem().string();
    }
    if (rallies_.empty()) {
        const cutting::CutListChoice choice =
            cutting::choose_cut_list(annotations_dir_, refined_dir_, detections_dir_, video_id_);
        rallies_ = choice.file;
        std::cout << std::format("Rallies: {} ({})\n", rallies_.string(), choice.kind);
        if (!std::filesystem::exists(rallies_)) {
            throw std::runtime_error(std::format(
                "{} does not exist: run detect first, or give the rallies with --rallies",
                rallies_.string()));
        }
    }
    if (fast_ && max_height_of(resolution_) > 0) {
        throw std::invalid_argument("--fast copies the video and cannot change its resolution; "
                                    "leave out --fast or --resolution");
    }
    const bool default_name = out_.empty();
    if (default_name) {
        out_ = out_dir_ / (video_id_ + ".mp4"); // the height is added once it is known
    }
    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, cache_dir_);
    // The cut list holds the rallies of the video id its file is named after
    annotation::CsvAnnotationRepository cut_list(rallies_.parent_path());
    cutting::FfmpegVideoCutter cutter;
    StreamProgressReporter progress(std::cout);
    cutting::CutVideo cut(probe, reader, cut_list, cutter, progress);

    const auto result = cut.execute(
        {.video = video_,
         .video_id = rallies_.stem().string(),
         .output = out_,
         .margins = {before_s_, after_s_},
         .options = {.mode = fast_ ? cutting::CutMode::Fast : cutting::CutMode::Exact,
                     .codec = codec_ == "h264" ? cutting::VideoCodec::H264
                                               : cutting::VideoCodec::H265,
                     .encoder = encoder_,
                     .bitrate_mbps = bitrate_mbps_,
                     .max_height = max_height_of(resolution_)},
         .resolution_in_name = default_name});
    std::cout << std::format(
        "Done in {}: {} rallies, {} of video ({}).\n", io::format_elapsed(result.seconds), result.rallies,
        io::format_clock(result.cut.duration_s),
        result.cut.video_encoder == "copy"
            ? "copied, cuts at keyframes"
            : std::format("encoded with {} at {:.1f} Mbit/s", result.cut.video_encoder,
                          result.cut.bitrate_mbps));
    std::cout << "Cut video: " << result.output.string() << '\n';
    return 0;
}

} // namespace ttrally::cli
