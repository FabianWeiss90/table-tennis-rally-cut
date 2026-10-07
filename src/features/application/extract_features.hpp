// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "features/application/feature_store.hpp"
#include "features/application/image_embedder.hpp"
#include "media/application/frame_decoder.hpp"
#include "media/application/media_probe.hpp"
#include "media/application/media_reader.hpp"
#include "shared/application/progress_reporter.hpp"

#include <filesystem>
#include <string>

namespace ttrally::features {

struct ExtractFeaturesRequest {
    std::filesystem::path video;
    std::string video_id;
    std::string video_fingerprint; ///< e.g. a hash of path, size and modification time
    double sample_rate_hz = 10.0;
    std::size_t batch_size = 16;
    media::DecodeBackend decode_backend = media::DecodeBackend::Auto;
    bool force = false; ///< Recompute even if current features are stored
};

struct ExtractFeaturesResult {
    bool skipped = false; ///< Stored features were already current
    std::size_t rows = 0;
    std::size_t dims = 0;
    double seconds = 0.0;
};

/// Use case behind `ttrally features`: samples the video on a regular time grid (10 per second
/// by default), runs every sampled frame through the image model and stores the feature rows
/// with their times and frame indices. Throws media::MediaError if the video cannot be read.
class ExtractFeatures {
  public:
    ExtractFeatures(media::MediaProbe& probe, media::MediaReader& reader,
                    media::FrameDecoderFactory& decoders, ImageEmbedder& embedder,
                    FeatureStore& store, ProgressReporter& progress);

    [[nodiscard]] ExtractFeaturesResult execute(const ExtractFeaturesRequest& request);

  private:
    media::MediaProbe& probe_;
    media::MediaReader& reader_;
    media::FrameDecoderFactory& decoders_;
    ImageEmbedder& embedder_;
    FeatureStore& store_;
    ProgressReporter& progress_;
};

} // namespace ttrally::features
