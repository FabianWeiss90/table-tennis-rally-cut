// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/application/detection_review_source.hpp"

#include <algorithm>

namespace ttrally::annotation {

std::vector<ReviewItem> DetectionReviewSource::load() {
    std::vector<RallyLabel> rallies = detections_.load(video_id_).rallies;
    if (rallies.empty()) {
        return {whole_video_gap(frame_count_)};
    }
    std::ranges::sort(rallies, {}, &RallyLabel::start_frame);
    std::vector<ReviewItem> items;
    int gap_id = 0;
    auto add_gap = [&](std::int64_t first, std::int64_t last) {
        if (first <= last) {
            items.push_back({ReviewKind::Gap, ++gap_id, first, last, ReviewStatus::Open});
        }
    };
    std::int64_t next_free = 0;
    for (std::size_t i = 0; i < rallies.size(); ++i) {
        add_gap(next_free, rallies[i].start_frame - 1);
        items.push_back({ReviewKind::Candidate, static_cast<int>(i) + 1, rallies[i].start_frame,
                         rallies[i].end_frame, ReviewStatus::Open});
        next_free = rallies[i].end_frame + 1;
    }
    add_gap(next_free, frame_count_ - 1);
    return items;
}

} // namespace ttrally::annotation
