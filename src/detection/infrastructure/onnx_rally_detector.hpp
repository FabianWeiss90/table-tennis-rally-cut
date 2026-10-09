// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "detection/application/rally_detector.hpp"

#include <filesystem>
#include <memory>

namespace ttrally::detection {

/// Rally detector exported by training/ttrally_training/train.py, run with ONNX Runtime on the
/// CPU (the model is small; a whole video takes seconds). Reads its description from the model
/// metadata; throws std::runtime_error if the file is not a rally detector.
class OnnxRallyDetector final : public RallyDetector {
  public:
    explicit OnnxRallyDetector(const std::filesystem::path& model);
    ~OnnxRallyDetector() override;
    OnnxRallyDetector(const OnnxRallyDetector&) = delete;
    OnnxRallyDetector& operator=(const OnnxRallyDetector&) = delete;

    [[nodiscard]] const DetectorInfo& info() const override;
    [[nodiscard]] std::vector<float> probabilities(std::span<const float> features,
                                                   std::size_t rows) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Decoding parameters as stored in the model metadata, e.g.
/// {"method": "threshold", "threshold": 0.5, "merge_gap_s": 1.0, "min_rally_s": 0.5}.
/// Throws std::runtime_error.
[[nodiscard]] DecodingParams parse_decoding(std::string_view json);

} // namespace ttrally::detection
