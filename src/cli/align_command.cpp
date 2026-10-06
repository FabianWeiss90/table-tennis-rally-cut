// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/align_command.hpp"

#include "alignment/infrastructure/csv_alignment_writer.hpp"
#include "alignment/infrastructure/html_report_writer.hpp"
#include "alignment/infrastructure/pocketfft_signal_matcher.hpp"
#include "cli/stream_progress_reporter.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_sampler.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <iostream>
#include <memory>

namespace ttrally::cli {

AlignCommand::AlignCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "align", "Align a cut video (e.g. from Liimba) against its original via the audio")) {
    auto& settings = request_.settings;
    command_->add_option("ORIGINAL", request_.original, "Untrimmed original video")
        ->required()
        ->check(CLI::ExistingFile);
    command_->add_option("CUT", request_.cut, "Cut video made from the original")
        ->required()
        ->check(CLI::ExistingFile);
    command_->add_option("--out", out_csv_,
                         "Segments CSV; gaps are written next to it as <name>.gaps.csv")
        ->required();
    command_->add_option("--report", report_, "Self-contained HTML report");
    command_->add_option("--cache-dir", cache_dir_, "Directory for decoded audio and timestamps")
        ->capture_default_str();
    command_->add_flag("--no-cache", no_cache_, "Do not read or write the cache");
    command_->add_option("--decode-backend", decode_backend_,
                         "Video decoding for the visual spot check")
        ->check(CLI::IsMember(media::decode_backend_names()))
        ->capture_default_str();
    command_->add_flag("--no-visual-check", no_visual_check_,
                       "Skip comparing one frame per segment in both videos");
    command_->add_option("--min-confidence", settings.min_confidence,
                         "Peak ratio below which a window counts as uncertain")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    command_->add_option("--local-search", settings.local_search_s,
                         "Seconds searched after the previous match before a global search")
        ->check(CLI::PositiveNumber)
        ->capture_default_str();
    command_->add_option("--threads", settings.threads, "Worker threads (0 = all cores)")
        ->capture_default_str();
}

int AlignCommand::run() {
    request_.visual_check = !no_visual_check_;
    request_.decode_backend = *media::parse_decode_backend(decode_backend_);

    // Composition root: concrete adapters for the use case's ports
    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    std::unique_ptr<media::CachingMediaReader> caching_reader;
    media::MediaReader* reader = &ffmpeg_reader;
    if (!no_cache_) {
        caching_reader = std::make_unique<media::CachingMediaReader>(ffmpeg_reader, cache_dir_);
        reader = caching_reader.get();
    }
    media::FfmpegFrameSamplerFactory frame_samplers;
    const alignment::PocketFftSignalMatcher matcher;
    StreamProgressReporter progress(std::cout);

    alignment::AlignVideos align_videos(probe, *reader, frame_samplers, matcher, progress);
    const alignment::AlignmentReport report = align_videos.execute(request_);

    for (const auto& warning : report.warnings) {
        std::cout << "  warning: " << warning << '\n';
    }
    const auto gaps_csv = alignment::gaps_path_for(out_csv_);
    alignment::write_segments_csv(out_csv_, report);
    alignment::write_gaps_csv(gaps_csv, report);
    std::cout << "Segments: " << out_csv_.string() << '\n'
              << "Gaps:     " << gaps_csv.string() << '\n';
    if (!report_.empty()) {
        alignment::write_html_report(report_, report);
        std::cout << "Report:   " << report_ << '\n';
    }
    return 0;
}

} // namespace ttrally::cli
