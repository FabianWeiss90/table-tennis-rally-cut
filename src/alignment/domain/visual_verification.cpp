// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/visual_verification.hpp"

#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>

namespace ttrally::alignment {

double image_similarity(const media::GrayImage& a, const media::GrayImage& b) {
    if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size()) {
        throw std::invalid_argument("images must have the same size");
    }
    const auto n = static_cast<double>(a.pixels.size());
    double sum_a = 0.0;
    double sum_b = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        sum_a += a.pixels[i];
        sum_b += b.pixels[i];
    }
    const double mean_a = sum_a / n;
    const double mean_b = sum_b / n;
    double covariance = 0.0;
    double variance_a = 0.0;
    double variance_b = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const double da = a.pixels[i] - mean_a;
        const double db = b.pixels[i] - mean_b;
        covariance += da * db;
        variance_a += da * da;
        variance_b += db * db;
    }
    if (variance_a <= 0.0 || variance_b <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return covariance / std::sqrt(variance_a * variance_b);
}

std::optional<std::string> assess_visual_check(const CandidateSegment& candidate,
                                               const VisualCheckPolicy& policy) {
    if (!candidate.visual_similarity) {
        return std::format("Segment {}: visual spot check could not compare frames.",
                           candidate.id);
    }
    if (*candidate.visual_similarity < policy.min_similarity) {
        return std::format("Segment {}: frames from the middle of the segment differ (image "
                           "correlation {:.2f}); check the alignment visually.",
                           candidate.id, *candidate.visual_similarity);
    }
    return std::nullopt;
}

} // namespace ttrally::alignment
