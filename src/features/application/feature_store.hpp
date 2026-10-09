// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::features {

/// Everything needed to decide whether stored features are still valid and to use them.
struct FeatureManifest {
    std::string video_id;
    std::string video_path;
    std::string video_fingerprint; ///< Changes whenever the video file changes
    std::string model_name;
    std::string model_fingerprint;
    std::vector<std::string> parts;
    std::size_t part_dims = 0;
    double sample_rate_hz = 0.0;
    int input_width = 0;
    int input_height = 0;
    Rational video_fps;
    std::int64_t video_frame_count = 0;
    std::size_t rows = 0;
    /// Where the model ran. Not part of the validity check, but recorded because float16 models
    /// give noticeably different features on different execution providers.
    std::string execution_provider;
};

/// Features of one video: one row per sample of the regular time grid.
struct FeatureSet {
    FeatureManifest manifest;
    std::vector<float> values;        ///< rows x dims, row by row
    std::vector<double> times_s;      ///< Grid time of each row
    std::vector<std::int64_t> frames; ///< Frame of the original used for each row
};

/// Port: persistent features, one set per video id.
class FeatureStore {
  public:
    virtual ~FeatureStore() = default;
    /// True if features with exactly this video, model and settings are already stored.
    [[nodiscard]] virtual bool is_current(const FeatureManifest& expected) = 0;
    virtual void save(const FeatureSet& features) = 0;
    /// The stored features of a video, nullopt if there are none. The fingerprints of video and
    /// model are not restored (they only decide whether features are current).
    [[nodiscard]] virtual std::optional<FeatureSet> load(const std::string& video_id) = 0;
};

} // namespace ttrally::features
