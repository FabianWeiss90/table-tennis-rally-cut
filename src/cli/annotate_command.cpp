// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/annotate_command.hpp"

#include "alignment/infrastructure/csv_alignment_writer.hpp"
#include "annotation/application/annotation_session.hpp"
#include "annotation/infrastructure/alignment_review_source.hpp"
#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "annotation/infrastructure/csv_review_state_store.hpp"
#include "gui/annotator_app.hpp"
#include "media/application/frame_prefetcher.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include <format>
#include <iostream>
#include <stdexcept>

namespace ttrally::cli {

AnnotateCommand::AnnotateCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "annotate", "Set exact rally start and end frames, starting from the align output")) {
    command_->add_option("ORIGINAL", original_, "Untrimmed original video")
        ->required()
        ->check(CLI::ExistingFile);
    command_->add_option("--segments", segments_csv_, "Segments CSV written by `ttrally align`")
        ->required()
        ->check(CLI::ExistingFile);
    command_->add_option("--gaps", gaps_csv_, "Gaps CSV (default: <segments>.gaps.csv)");
    command_->add_option("--video-id", video_id_,
                         "Video id in the label file (default: name of the segments CSV)");
    command_->add_option("--annotations-dir", annotations_dir_, "Directory of the label files")
        ->capture_default_str();
    command_->add_option("--state-dir", state_dir_, "Directory for the review progress")
        ->capture_default_str();
    command_->add_option("--cache-dir", cache_dir_, "Cache for frame timestamps")
        ->capture_default_str();
    command_->add_option("--decode-backend", decode_backend_, "Video decoding")
        ->check(CLI::IsMember(media::decode_backend_names()))
        ->capture_default_str();
    command_->add_option("--display-height", display_height_,
                         "Height frames are decoded at for display")
        ->check(CLI::Range(144, 4320))
        ->capture_default_str();
    command_->add_option("--frame-memory", frame_memory_mb_,
                         "Memory for decoded frames in MB (more allows longer steps back)")
        ->check(CLI::Range(64, 65536))
        ->capture_default_str();
}

int AnnotateCommand::run() {
    if (gaps_csv_.empty()) {
        gaps_csv_ = alignment::gaps_path_for(segments_csv_);
    }
    if (video_id_.empty()) {
        video_id_ = segments_csv_.stem().string();
    }

    // Original video: frame rate, frame count and timestamps
    media::FfmpegMediaProbe probe;
    const media::MediaInfo info = probe.probe(original_);
    if (!info.video) {
        throw std::runtime_error(original_.string() + " has no video stream");
    }
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, cache_dir_);
    std::cout << "Reading frame timestamps...\n";
    const media::VideoTimestamps timestamps =
        *reader.read(original_, {.audio_sample_rate = std::nullopt,
                                 .video_stream_index = info.video->index})
             .video_timestamps;

    // Annotation session with its repositories
    annotation::CsvAnnotationRepository annotations(annotations_dir_);
    annotation::AlignmentReviewSource items(segments_csv_, gaps_csv_);
    annotation::CsvReviewStateStore states(state_dir_);
    const Rational fps = timestamps.timeline(info.video->nominal_frame_rate()).nominal_fps();
    annotation::AnnotationSession session({video_id_, fps, timestamps.frame_count()}, annotations,
                                          items, states);

    // Frame access for display
    media::FfmpegFrameDecoderFactory decoders;
    auto decoder = decoders.open(original_, timestamps,
                                 *media::parse_decode_backend(decode_backend_),
                                 {.height = display_height_});
    const std::size_t frame_bytes = media::VideoFrame::bytes_for(decoder->frame_size());
    const std::size_t capacity =
        static_cast<std::size_t>(frame_memory_mb_) * 1024 * 1024 / frame_bytes;
    media::FramePrefetcher frames(std::move(decoder), capacity);

    std::cout << std::format("Labels: {}\n", annotations.file_for(video_id_).string());
    gui::AnnotatorOptions options;
    options.title = "ttrally annotate - " + video_id_;
    gui::run_annotator(session, frames, fps, options);
    return 0;
}

} // namespace ttrally::cli
