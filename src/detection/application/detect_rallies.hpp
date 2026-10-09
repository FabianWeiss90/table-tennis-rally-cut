// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"
#include "detection/application/detection_output.hpp"
#include "detection/application/rally_detector.hpp"
#include "detection/domain/evaluation.hpp"
#include "features/application/extract_features.hpp"
#include "shared/application/progress_reporter.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ttrally::detection {

/// The detector cannot be used with these features (another image model, other settings).
class IncompatibleFeatures : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

struct DetectRalliesRequest {
    features::ExtractFeaturesRequest features; ///< How missing features are computed
    bool evaluate = true; ///< Compare with the labels of the video, if there are any
};

/// Comparison with the annotated rallies of the video.
struct DetectionEvaluation {
    SegmentMetrics metrics;
    bool labels_complete = false; ///< The review of the labels was finished in annotate
    std::size_t ignored_rows = 0;
};

struct DetectRalliesResult {
    Detection detection;
    std::optional<DetectionEvaluation> evaluation;
    std::vector<std::string> warnings;
};

/// Use case behind `ttrally detect`: makes sure the video has features of the image model the
/// detector was trained on (computing them if needed), runs the detector, decodes rallies with
/// the decoding stored in the detector, saves them and, if the video has labels, evaluates them.
class DetectRallies {
  public:
    /// `labels` and `reviews` may be null (no evaluation).
    DetectRallies(features::ExtractFeatures& extract, const features::ImageEmbedder& backbone,
                  features::FeatureStore& store, RallyDetector& detector,
                  annotation::AnnotationRepository* labels,
                  annotation::ReviewStateStore* reviews, DetectionOutput& output,
                  ProgressReporter& progress);

    /// Throws IncompatibleFeatures, media::MediaError or std::runtime_error.
    [[nodiscard]] DetectRalliesResult execute(const DetectRalliesRequest& request);

  private:
    void check_backbone() const;
    [[nodiscard]] std::vector<std::string> check(const features::FeatureManifest& manifest) const;
    [[nodiscard]] std::optional<DetectionEvaluation>
    evaluate(const std::string& video_id, const FrameGrid& grid,
             const std::vector<RowSegment>& detected, double sample_rate_hz);

    features::ExtractFeatures& extract_;
    const features::ImageEmbedder& backbone_;
    features::FeatureStore& store_;
    RallyDetector& detector_;
    annotation::AnnotationRepository* labels_;
    annotation::ReviewStateStore* reviews_;
    DetectionOutput& output_;
    ProgressReporter& progress_;
};

} // namespace ttrally::detection
