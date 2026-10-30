// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"

#include <cstdint>
#include <string>

namespace ttrally::annotation {

/// Review items for refining detected rallies: every detected rally is a candidate, every
/// stretch between them (and before the first, after the last) a gap that may hide a missed
/// rally. Without detections the whole video is one gap.
class DetectionReviewSource final : public ReviewItemSource {
  public:
    DetectionReviewSource(AnnotationRepository& detections, std::string video_id,
                          std::int64_t frame_count)
        : detections_(detections), video_id_(std::move(video_id)), frame_count_(frame_count) {}

    [[nodiscard]] std::vector<ReviewItem> load() override;

  private:
    AnnotationRepository& detections_;
    std::string video_id_;
    std::int64_t frame_count_;
};

} // namespace ttrally::annotation
