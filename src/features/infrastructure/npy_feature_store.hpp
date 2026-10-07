// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "features/application/feature_store.hpp"

#include <filesystem>

namespace ttrally::features {

/// Stores features in <directory>/<video_id>/:
///   features.npy   float32, rows x dims
///   times.npy      float64, grid time of each row (seconds on the video's timeline)
///   frames.npy     int64, frame of the original used for each row
///   manifest.json  video, model and settings the features were computed with
class NpyFeatureStore final : public FeatureStore {
  public:
    explicit NpyFeatureStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

    [[nodiscard]] bool is_current(const FeatureManifest& expected) override;
    void save(const FeatureSet& features) override;

    [[nodiscard]] std::filesystem::path directory_for(const std::string& video_id) const {
        return directory_ / video_id;
    }

  private:
    std::filesystem::path directory_;
};

/// Hash of everything in the manifest that determines the feature values.
[[nodiscard]] std::string manifest_fingerprint(const FeatureManifest& manifest);

/// The manifest as a JSON document.
[[nodiscard]] std::string manifest_json(const FeatureManifest& manifest);

} // namespace ttrally::features
