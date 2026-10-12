// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/cut_command.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "cli/stream_progress_reporter.hpp"
#include "cutting/application/cut_video.hpp"
#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/formatting.hpp"

#include <format>
#include <iostream>

namespace ttrally::cli {

CutCommand::CutCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "cut", "Cut a video down to its rallies, with a margin before and after each")) {
    command_->add_option("VIDEO", video_, "Original video")->required()->check(CLI::ExistingFile);
    command_->add_option("--video-id", video_id_,
                         "Video id (default: file name without extension)");
    command_->add_option("--rallies", rallies_,
                         "Cut list in the label format (default: data/detections/<video-id>.csv; "
                         "annotations work too)")
        ->check(CLI::ExistingFile);
    command_->add_option("--out", out_, "Output video (default: data/cuts/<video-id>.mp4)");
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
                         "first of the codec that works, GPU first)")
        ->capture_default_str();
    command_->add_option("--bitrate", bitrate_mbps_,
                         "Video bit rate in Mbit/s (default: by resolution and frame rate; for "
                         "Full HD at 60 fps 5 with H.265, 10 with H.264)")
        ->check(CLI::PositiveNumber);
    command_->add_option("--cache-dir", cache_dir_, "Cache for frame timestamps")
        ->capture_default_str();
}

int CutCommand::run() {
    if (video_id_.empty()) {
        video_id_ = video_.stem().string();
    }
    if (rallies_.empty()) {
        rallies_ = std::filesystem::path("data/detections") / (video_id_ + ".csv");
    }
    if (out_.empty()) {
        out_ = std::filesystem::path("data/cuts") / (video_id_ + ".mp4");
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
                     .bitrate_mbps = bitrate_mbps_}});
    std::cout << std::format(
        "Done in {:.0f} s: {} rallies, {} of video ({}).\n", result.seconds, result.rallies,
        io::format_clock(result.cut.duration_s),
        result.cut.video_encoder == "copy"
            ? "copied, cuts at keyframes"
            : std::format("encoded with {} at {:.0f} Mbit/s", result.cut.video_encoder,
                          result.cut.bitrate_mbps));
    std::cout << "Cut video: " << out_.string() << '\n';
    return 0;
}

} // namespace ttrally::cli
