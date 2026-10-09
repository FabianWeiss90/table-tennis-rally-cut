// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "detection/domain/frame_grid.hpp"
#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ttrally::detection {

/// A detected rally in frames of the original.
struct DetectedRally {
    FrameSpan frames;
    double start_s = 0.0;
    double end_s = 0.0;
    double mean_probability = 0.0;
};

/// Everything a detection run produces for one video.
struct Detection {
    std::string video_id;
    Rational video_fps;
    std::int64_t video_frame_count = 0;
    std::vector<DetectedRally> rallies;
    std::vector<float> probabilities; ///< one per feature row
};

/// Port: where detections are written.
class DetectionOutput {
  public:
    virtual ~DetectionOutput() = default;
    virtual void save(const Detection& detection) = 0;
    /// Human-readable location of the saved detection (for messages).
    [[nodiscard]] virtual std::string location(const std::string& video_id) const = 0;
};

} // namespace ttrally::detection
