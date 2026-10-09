// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
// Adapted from spin-detector by Yuwei Ba (MIT License)
// https://github.com/ibigbug/spin-detector
// Original file: src/evaluation/metrics.py (SegmentMetrics, evaluate)
// Changes: ported to C++ via training/ttrally_training/metrics.py; boundary errors in seconds;
//   ignored sections.

#pragma once

#include "detection/domain/row_segment.hpp"

#include <cstddef>
#include <vector>

namespace ttrally::detection {

/// Detected rallies compared with the annotated ones (same definitions as in training).
struct SegmentMetrics {
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
    double mean_iou = 0.0;       ///< over matched pairs
    double boundary_mae_s = 0.0; ///< mean of |start error| and |end error| over matched pairs
    double start_mae_s = 0.0;
    double end_mae_s = 0.0;
    double row_precision = 0.0;
    double row_recall = 0.0;
    double row_f1 = 0.0;
    std::size_t annotated = 0;
    std::size_t predicted = 0;
    std::size_t matched = 0;
};

/// Minimum IoU for a detected rally to match an annotated one.
inline constexpr double kIouThreshold = 0.5;

/// Segment level: a detection matches an unmatched annotated rally if their IoU reaches the
/// threshold (greedy, in detection order). Row level: precision, recall and F1 of the rally
/// rows. `ignored` (one value per row, may be empty) marks ignored sections: detections lying
/// mostly inside them are dropped, and their rows do not count.
[[nodiscard]] SegmentMetrics evaluate(std::vector<RowSegment> detected,
                                      const std::vector<RowSegment>& annotated, std::size_t rows,
                                      double sample_rate_hz, const std::vector<bool>& ignored = {});

} // namespace ttrally::detection
