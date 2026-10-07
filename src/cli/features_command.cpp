// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/features_command.hpp"

#include "cli/stream_progress_reporter.hpp"
#include "features/application/extract_features.hpp"
#include "features/infrastructure/npy_feature_store.hpp"
#include "features/infrastructure/onnx_image_embedder.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/file_cache.hpp"

#include <format>
#include <iostream>

namespace ttrally::cli {

FeaturesCommand::FeaturesCommand(CLI::App& app)
    : command_(app.add_subcommand("features",
                                  "Compute per-frame image features of a video (10 per second)")) {
    command_->add_option("VIDEO", video_, "Original video")->required()->check(CLI::ExistingFile);
    command_->add_option("--video-id", video_id_,
                         "Name of the output directory (default: file name without extension)");
    command_->add_option("--model", model_, "Image model exported by training/ttrally_training")
        ->check(CLI::ExistingFile)
        ->capture_default_str();
    command_->add_option("--out-dir", out_dir_, "Features go to <out-dir>/<video-id>/")
        ->capture_default_str();
    command_->add_option("--cache-dir", cache_dir_, "Cache for frame timestamps")
        ->capture_default_str();
    command_->add_option("--ep", execution_provider_, "Where the image model runs")
        ->check(CLI::IsMember(features::execution_provider_names()))
        ->capture_default_str();
    command_->add_option("--decode-backend", decode_backend_, "Video decoding")
        ->check(CLI::IsMember(media::decode_backend_names()))
        ->capture_default_str();
    command_->add_option("--rate", sample_rate_hz_, "Samples per second")
        ->check(CLI::Range(0.5, 60.0))
        ->capture_default_str();
    command_->add_option("--batch", batch_size_, "Images per model run")
        ->check(CLI::Range(1, 256))
        ->capture_default_str();
    command_->add_flag("--force", force_, "Recompute even if the stored features are current");
}

int FeaturesCommand::run() {
    if (video_id_.empty()) {
        video_id_ = video_.stem().string();
    }
    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, cache_dir_);
    media::FfmpegFrameDecoderFactory decoders;
    features::OnnxImageEmbedder embedder(model_,
                                         *features::parse_execution_provider(execution_provider_));
    features::NpyFeatureStore store(out_dir_);
    StreamProgressReporter progress(std::cout);

    features::ExtractFeatures extract(probe, reader, decoders, embedder, store, progress);
    const auto result = extract.execute({.video = video_,
                                         .video_id = video_id_,
                                         .video_fingerprint = io::cache_key(video_, "video"),
                                         .sample_rate_hz = sample_rate_hz_,
                                         .batch_size = batch_size_,
                                         .decode_backend = *media::parse_decode_backend(
                                             decode_backend_),
                                         .force = force_});
    if (!result.skipped) {
        std::cout << std::format("Done in {:.1f} s: {} rows x {} values\n", result.seconds,
                                 result.rows, result.dims);
    }
    std::cout << "Features: " << store.directory_for(video_id_).string() << '\n';
    return 0;
}

} // namespace ttrally::cli
