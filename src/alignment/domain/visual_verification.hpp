// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/candidates.hpp"
#include "media/domain/gray_image.hpp"

#include <optional>
#include <string>

namespace ttrally::alignment {

/// Visual spot check: one frame from the middle of each candidate is compared in both videos.
struct VisualCheckPolicy {
    int width = 64;              ///< Frames are compared downscaled to this size
    int height = 36;
    double min_similarity = 0.7; ///< Image correlation below this is reported
};

/// Pearson correlation of two images of equal size; NaN if either image is constant.
[[nodiscard]] double image_similarity(const media::GrayImage& a, const media::GrayImage& b);

/// Warning for a candidate whose spot check failed or could not be done, otherwise nullopt.
[[nodiscard]] std::optional<std::string> assess_visual_check(const CandidateSegment& candidate,
                                                             const VisualCheckPolicy& policy);

} // namespace ttrally::alignment
