// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "detection/domain/decoding.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ttrally::detection {

/// Description of a trained rally detector, read from the model itself: which features it
/// expects and how its probabilities are decoded.
struct DetectorInfo {
    std::string backbone;                    ///< Image model of the features, e.g. "... (fp16)"
    std::string backbone_execution_provider; ///< Where the training features were computed
    std::vector<std::string> parts;
    std::size_t part_dims = 0;
    int input_width = 0;
    int input_height = 0;
    double sample_rate_hz = 0.0;
    DecodingParams decoding;

    [[nodiscard]] std::size_t dims() const noexcept { return parts.size() * part_dims; }
};

/// Port: per-row rally probabilities of a whole video's features.
class RallyDetector {
  public:
    virtual ~RallyDetector() = default;
    [[nodiscard]] virtual const DetectorInfo& info() const = 0;
    /// `features` holds rows * info().dims() values, row by row; returns one value per row.
    [[nodiscard]] virtual std::vector<float> probabilities(std::span<const float> features,
                                                           std::size_t rows) = 0;
};

} // namespace ttrally::detection
