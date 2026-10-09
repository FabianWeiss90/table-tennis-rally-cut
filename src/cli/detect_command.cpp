// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/detect_command.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "annotation/infrastructure/csv_review_state_store.hpp"
#include "cli/stream_progress_reporter.hpp"
#include "detection/application/detect_rallies.hpp"
#include "detection/infrastructure/csv_detection_output.hpp"
#include "detection/infrastructure/onnx_rally_detector.hpp"
#include "features/infrastructure/npy_feature_store.hpp"
#include "features/infrastructure/onnx_image_embedder.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/file_cache.hpp"
#include "shared/io/formatting.hpp"

#include <format>
#include <iostream>

namespace ttrally::cli {

namespace {

void print_rallies(const detection::Detection& detection) {
    std::cout << std::format("{} rallies:\n", detection.rallies.size());
    int number = 0;
    for (const auto& rally : detection.rallies) {
        std::cout << std::format("  {:3}  {} - {}  ({:4.1f} s)  frames {}-{}  p={:.2f}\n",
                                 ++number, io::format_clock(rally.start_s),
                                 io::format_clock(rally.end_s), rally.end_s - rally.start_s,
                                 rally.frames.start_frame, rally.frames.end_frame,
                                 rally.mean_probability);
    }
}

void print_evaluation(const detection::DetectionEvaluation& evaluation) {
    const auto& m = evaluation.metrics;
    std::cout << std::format(
        "Compared with the labels: {} of {} rallies found, {} detected\n"
        "  Seg-F1 {:.3f}  precision {:.3f}  recall {:.3f}  boundaries {:.2f} s "
        "(start {:.2f} s, end {:.2f} s)\n",
        m.matched, m.annotated, m.predicted, m.f1, m.precision, m.recall, m.boundary_mae_s,
        m.start_mae_s, m.end_mae_s);
    if (evaluation.ignored_rows > 0) {
        std::cout << "  Ignored sections are left out.\n";
    }
    if (!evaluation.labels_complete) {
        std::cout << "  Note: the review of these labels is not complete, so the numbers may "
                     "be off.\n";
    }
}

} // namespace

DetectCommand::DetectCommand(CLI::App& app)
    : command_(app.add_subcommand("detect", "Find the rallies of a video with a trained detector")) {
    command_->add_option("VIDEO", video_, "Original video")->required()->check(CLI::ExistingFile);
    command_->add_option("--video-id", video_id_,
                         "Name for features, output and labels (default: file name without "
                         "extension)");
    command_->add_option("--detector", detector_, "Rally detector exported by the training")
        ->check(CLI::ExistingFile)
        ->capture_default_str();
    command_->add_option("--backbone", backbone_,
                         "Image model for missing features; must be the one the detector was "
                         "trained with")
        ->check(CLI::ExistingFile)
        ->capture_default_str();
    command_->add_option("--features-dir", features_dir_, "Stored features")
        ->capture_default_str();
    command_->add_option("--cache-dir", cache_dir_, "Cache for frame timestamps")
        ->capture_default_str();
    command_->add_option("--out-dir", out_dir_,
                         "Detections go to <out-dir>/<video-id>.csv (label format)")
        ->capture_default_str();
    command_->add_option("--labels-dir", labels_dir_,
                         "Labels to compare with, if the video has any")
        ->capture_default_str();
    command_->add_flag("--no-evaluation", no_evaluation_, "Do not compare with labels");
    command_->add_option("--ep", execution_provider_, "Where the image model runs")
        ->check(CLI::IsMember(features::execution_provider_names()))
        ->capture_default_str();
    command_->add_option("--decode-backend", decode_backend_, "Video decoding")
        ->check(CLI::IsMember(media::decode_backend_names()))
        ->capture_default_str();
    command_->add_option("--batch", batch_size_, "Images per model run")
        ->check(CLI::Range(1, 256))
        ->capture_default_str();
}

int DetectCommand::run() {
    if (video_id_.empty()) {
        video_id_ = video_.stem().string();
    }
    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, cache_dir_);
    media::FfmpegFrameDecoderFactory decoders;
    features::OnnxImageEmbedder backbone(backbone_,
                                         *features::parse_execution_provider(execution_provider_));
    features::NpyFeatureStore store(features_dir_);
    StreamProgressReporter progress(std::cout);
    features::ExtractFeatures extract(probe, reader, decoders, backbone, store, progress);

    detection::OnnxRallyDetector detector(detector_);
    annotation::CsvAnnotationRepository labels(labels_dir_);
    annotation::CsvReviewStateStore reviews(labels_dir_);
    detection::CsvDetectionOutput output(out_dir_);
    detection::DetectRallies detect(extract, backbone, store, detector,
                                    no_evaluation_ ? nullptr : &labels,
                                    no_evaluation_ ? nullptr : &reviews, output, progress);

    const auto result = detect.execute(
        {.features = {.video = video_,
                      .video_id = video_id_,
                      .video_fingerprint = io::cache_key(video_, "video"),
                      .batch_size = batch_size_,
                      .decode_backend = *media::parse_decode_backend(decode_backend_)},
         .evaluate = !no_evaluation_});
    for (const auto& warning : result.warnings) {
        std::cout << "Warning: " << warning << '\n';
    }
    print_rallies(result.detection);
    if (result.evaluation) {
        print_evaluation(*result.evaluation);
    }
    std::cout << "Detections: " << output.location(video_id_) << '\n';
    return 0;
}

} // namespace ttrally::cli
