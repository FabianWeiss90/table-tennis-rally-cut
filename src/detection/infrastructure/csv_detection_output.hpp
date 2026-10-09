// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "detection/application/detection_output.hpp"

#include <filesystem>

namespace ttrally::detection {

/// Writes detections next to each other in <directory>:
///   <video_id>.csv                 the rallies in the label format, so `ttrally annotate
///                                  --annotations-dir <directory>` can open them for checking
///                                  (notes: mean rally probability)
///   <video_id>.probabilities.npy   float32, rally probability of every feature row
class CsvDetectionOutput final : public DetectionOutput {
  public:
    explicit CsvDetectionOutput(std::filesystem::path directory)
        : directory_(std::move(directory)) {}

    void save(const Detection& detection) override;
    [[nodiscard]] std::string location(const std::string& video_id) const override;

  private:
    std::filesystem::path directory_;
};

} // namespace ttrally::detection
