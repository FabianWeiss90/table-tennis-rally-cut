// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
// Adapted from spin-detector by Yuwei Ba (MIT License)
// https://github.com/ibigbug/spin-detector
// Original file: src/evaluation/metrics.py (SegmentMetrics, evaluate)
// Changes: ported to C++ via training/ttrally_training/metrics.py; boundary errors in seconds;
//   ignored sections.

#include "detection/domain/evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace ttrally::detection {

namespace {

constexpr double kMaxIgnoredShare = 0.5; ///< detections with more of their rows ignored drop

double f1_of(double precision, double recall) {
    return precision + recall > 0 ? 2 * precision * recall / (precision + recall) : 0.0;
}

double mean_of(const std::vector<double>& values) {
    return values.empty() ? 0.0
                          : std::accumulate(values.begin(), values.end(), 0.0) /
                                static_cast<double>(values.size());
}

std::vector<bool> mask_of(const std::vector<RowSegment>& segments, std::size_t rows) {
    std::vector<bool> mask(rows, false);
    for (const RowSegment& segment : segments) {
        for (auto row = std::max<std::int64_t>(0, segment.first);
             row <= segment.last && row < static_cast<std::int64_t>(rows); ++row) {
            mask[static_cast<std::size_t>(row)] = true;
        }
    }
    return mask;
}

double ignored_share(const RowSegment& segment, const std::vector<bool>& ignored) {
    std::int64_t count = 0;
    for (std::int64_t row = segment.first; row <= segment.last; ++row) {
        count += ignored[static_cast<std::size_t>(row)] ? 1 : 0;
    }
    return static_cast<double>(count) / static_cast<double>(segment.length());
}

} // namespace

SegmentMetrics evaluate(std::vector<RowSegment> detected, const std::vector<RowSegment>& annotated,
                        std::size_t rows, double sample_rate_hz,
                        const std::vector<bool>& ignored) {
    if (!ignored.empty()) {
        std::erase_if(detected, [&](const RowSegment& segment) {
            return ignored_share(segment, ignored) > kMaxIgnoredShare;
        });
    }
    std::vector<bool> matched(annotated.size(), false);
    std::vector<double> ious;
    std::vector<double> start_errors;
    std::vector<double> end_errors;
    for (const RowSegment& detection : detected) {
        double best_iou = 0.0;
        std::size_t best = annotated.size();
        for (std::size_t i = 0; i < annotated.size(); ++i) {
            const double iou = detection.iou(annotated[i]);
            if (!matched[i] && iou > best_iou) {
                best_iou = iou;
                best = i;
            }
        }
        if (best < annotated.size() && best_iou >= kIouThreshold) {
            matched[best] = true;
            ious.push_back(best_iou);
            start_errors.push_back(
                static_cast<double>(std::abs(detection.first - annotated[best].first)) /
                sample_rate_hz);
            end_errors.push_back(
                static_cast<double>(std::abs(detection.last - annotated[best].last)) /
                sample_rate_hz);
        }
    }

    SegmentMetrics metrics;
    metrics.annotated = annotated.size();
    metrics.predicted = detected.size();
    metrics.matched = ious.size();
    metrics.precision = detected.empty() ? 0.0
                                         : static_cast<double>(ious.size()) /
                                               static_cast<double>(detected.size());
    metrics.recall = annotated.empty() ? 0.0
                                       : static_cast<double>(ious.size()) /
                                             static_cast<double>(annotated.size());
    metrics.f1 = f1_of(metrics.precision, metrics.recall);
    metrics.mean_iou = mean_of(ious);
    std::vector<double> boundary_errors(start_errors.size());
    std::ranges::transform(start_errors, end_errors, boundary_errors.begin(),
                           [](double s, double e) { return (s + e) / 2; });
    metrics.boundary_mae_s = mean_of(boundary_errors);
    metrics.start_mae_s = mean_of(start_errors);
    metrics.end_mae_s = mean_of(end_errors);

    const std::vector<bool> detected_rows = mask_of(detected, rows);
    const std::vector<bool> annotated_rows = mask_of(annotated, rows);
    std::size_t true_positive = 0;
    std::size_t detected_count = 0;
    std::size_t annotated_count = 0;
    for (std::size_t row = 0; row < rows; ++row) {
        if (!ignored.empty() && ignored[row]) {
            continue;
        }
        detected_count += detected_rows[row] ? 1 : 0;
        annotated_count += annotated_rows[row] ? 1 : 0;
        true_positive += detected_rows[row] && annotated_rows[row] ? 1 : 0;
    }
    metrics.row_precision = static_cast<double>(true_positive) /
                            static_cast<double>(std::max<std::size_t>(1, detected_count));
    metrics.row_recall = static_cast<double>(true_positive) /
                         static_cast<double>(std::max<std::size_t>(1, annotated_count));
    metrics.row_f1 = f1_of(metrics.row_precision, metrics.row_recall);
    return metrics;
}

} // namespace ttrally::detection
