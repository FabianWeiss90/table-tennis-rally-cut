// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "detection/application/detect_rallies.hpp"

#include <algorithm>
#include <format>
#include <numeric>
#include <stdexcept>

namespace ttrally::detection {

namespace {

constexpr double kRateTolerance = 1e-9;

std::vector<FrameSpan> spans_of(const std::vector<annotation::RallyLabel>& rallies) {
    std::vector<FrameSpan> spans;
    spans.reserve(rallies.size());
    for (const auto& rally : rallies) {
        spans.push_back({rally.start_frame, rally.end_frame});
    }
    return spans;
}

std::vector<FrameSpan> spans_of(const std::vector<annotation::IgnoredSection>& sections) {
    std::vector<FrameSpan> spans;
    spans.reserve(sections.size());
    for (const auto& section : sections) {
        spans.push_back({section.start_frame, section.end_frame});
    }
    return spans;
}

std::string joined(const std::vector<std::string>& values) {
    std::string text;
    for (const auto& value : values) {
        text += (text.empty() ? "" : ", ") + value;
    }
    return text;
}

} // namespace

DetectRallies::DetectRallies(features::ExtractFeatures& extract,
                             const features::ImageEmbedder& backbone,
                             features::FeatureStore& store, RallyDetector& detector,
                             annotation::AnnotationRepository* labels,
                             annotation::ReviewStateStore* reviews, DetectionOutput& output,
                             ProgressReporter& progress)
    : extract_(extract), backbone_(backbone), store_(store), detector_(detector), labels_(labels),
      reviews_(reviews), output_(output), progress_(progress) {}

DetectRalliesResult DetectRallies::execute(const DetectRalliesRequest& request) {
    const DetectorInfo& info = detector_.info();
    check_backbone();
    features::ExtractFeaturesRequest features_request = request.features;
    features_request.sample_rate_hz = info.sample_rate_hz; // the grid the detector was trained on
    static_cast<void>(extract_.execute(features_request));

    const std::string& video_id = request.features.video_id;
    const auto features = store_.load(video_id);
    if (!features) {
        throw std::runtime_error("no features were stored for " + video_id);
    }
    DetectRalliesResult result;
    result.warnings = check(features->manifest);
    const features::FeatureManifest& manifest = features->manifest;

    progress_.report(std::format("Detecting rallies ({})...", describe(info.decoding)));
    std::vector<float> probabilities = detector_.probabilities(features->values, manifest.rows);
    if (probabilities.size() != manifest.rows) {
        throw std::runtime_error("the detector returned a wrong number of probabilities");
    }
    const std::vector<RowSegment> segments =
        decode(probabilities, manifest.sample_rate_hz, info.decoding);
    const FrameGrid grid(features->frames, manifest.video_frame_count);

    Detection& detection = result.detection;
    detection.video_id = video_id;
    detection.video_fps = manifest.video_fps;
    detection.video_frame_count = manifest.video_frame_count;
    for (const RowSegment& segment : segments) {
        const auto first = static_cast<std::size_t>(segment.first);
        const auto end = static_cast<std::size_t>(segment.last) + 1;
        const double sum = std::accumulate(probabilities.begin() + static_cast<std::ptrdiff_t>(first),
                                           probabilities.begin() + static_cast<std::ptrdiff_t>(end),
                                           0.0);
        detection.rallies.push_back(
            {grid.frames_of(segment), features->times_s[first],
             features->times_s[end - 1] + 1.0 / manifest.sample_rate_hz,
             sum / static_cast<double>(end - first)});
    }
    detection.probabilities = std::move(probabilities);
    output_.save(detection);

    if (request.evaluate) {
        result.evaluation = evaluate(video_id, grid, segments, manifest.sample_rate_hz);
    }
    return result;
}

void DetectRallies::check_backbone() const {
    const DetectorInfo& info = detector_.info();
    const features::EmbedderInfo& image_model = backbone_.info();
    if (image_model.model_name != info.backbone) {
        throw IncompatibleFeatures(std::format(
            "the detector was trained on features of \"{}\", but the image model is \"{}\"; "
            "pass the image model the detector was trained with (--backbone)",
            info.backbone, image_model.model_name));
    }
    if (image_model.parts != info.parts || image_model.part_dims != info.part_dims ||
        image_model.input.size.width != info.input_width ||
        image_model.input.size.height != info.input_height) {
        throw IncompatibleFeatures("the image model describes its features differently than "
                                   "the one the detector was trained with");
    }
}

std::vector<std::string> DetectRallies::check(const features::FeatureManifest& manifest) const {
    const DetectorInfo& info = detector_.info();
    std::vector<std::string> mismatches;
    if (manifest.model_name != info.backbone) {
        mismatches.push_back(std::format("image model \"{}\" instead of \"{}\"",
                                         manifest.model_name, info.backbone));
    }
    if (manifest.parts != info.parts || manifest.part_dims != info.part_dims) {
        mismatches.emplace_back("other feature parts");
    }
    if (std::abs(manifest.sample_rate_hz - info.sample_rate_hz) > kRateTolerance) {
        mismatches.push_back(std::format("{} instead of {} samples per second",
                                         manifest.sample_rate_hz, info.sample_rate_hz));
    }
    if (manifest.input_width != info.input_width || manifest.input_height != info.input_height) {
        mismatches.emplace_back("another image size");
    }
    if (!mismatches.empty()) {
        throw IncompatibleFeatures("the stored features do not fit the detector: " +
                                   joined(mismatches));
    }
    std::vector<std::string> warnings;
    if (!info.backbone_execution_provider.empty() &&
        manifest.execution_provider != info.backbone_execution_provider) {
        warnings.push_back(std::format(
            "the features were computed on {}, the training features on {}; with float16 models "
            "the values differ slightly, which may cost some accuracy",
            manifest.execution_provider.empty() ? "an unknown provider"
                                                : manifest.execution_provider,
            info.backbone_execution_provider));
    }
    return warnings;
}

std::optional<DetectionEvaluation> DetectRallies::evaluate(const std::string& video_id,
                                                           const FrameGrid& grid,
                                                           const std::vector<RowSegment>& detected,
                                                           double sample_rate_hz) {
    if (labels_ == nullptr) {
        return std::nullopt;
    }
    const annotation::StoredLabels labels = labels_->load(video_id);
    if (labels.rallies.empty() && labels.ignored.empty()) {
        return std::nullopt;
    }
    const auto rally_spans = spans_of(labels.rallies);
    const auto ignored_spans = spans_of(labels.ignored);
    const std::vector<bool> ignored = grid.rows_inside(ignored_spans);

    DetectionEvaluation evaluation;
    evaluation.metrics = detection::evaluate(detected, segments_of(grid.rows_inside(rally_spans)),
                                             grid.rows(), sample_rate_hz, ignored);
    evaluation.ignored_rows = static_cast<std::size_t>(std::ranges::count(ignored, true));
    if (reviews_ != nullptr) {
        const annotation::ReviewStatusMap statuses = reviews_->load(video_id);
        evaluation.labels_complete =
            !statuses.empty() && std::ranges::none_of(statuses, [](const auto& entry) {
                return entry.second == annotation::ReviewStatus::Open;
            });
    }
    return evaluation;
}

} // namespace ttrally::detection
