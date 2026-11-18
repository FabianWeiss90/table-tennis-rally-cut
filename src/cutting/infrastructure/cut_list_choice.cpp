// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/cut_list_choice.hpp"

#include "annotation/infrastructure/csv_review_state_store.hpp"

#include <algorithm>

namespace ttrally::cutting {

namespace {

/// The labels of a video are complete once every segment and gap of its review is done.
bool labels_complete(const std::filesystem::path& annotations_dir, const std::string& video_id) {
    if (!std::filesystem::exists(annotations_dir / (video_id + ".csv"))) {
        return false;
    }
    const auto statuses = annotation::CsvReviewStateStore(annotations_dir).load(video_id);
    return !statuses.empty() && std::ranges::none_of(statuses, [](const auto& entry) {
        return entry.second == annotation::ReviewStatus::Open;
    });
}

} // namespace

CutListChoice choose_cut_list(const std::filesystem::path& annotations_dir,
                              const std::filesystem::path& refined_dir,
                              const std::filesystem::path& detections_dir,
                              const std::string& video_id) {
    const std::string file = video_id + ".csv";
    if (labels_complete(annotations_dir, video_id)) {
        return {annotations_dir / file, "complete annotations"};
    }
    if (std::filesystem::exists(refined_dir / file)) {
        return {refined_dir / file, "refined detections"};
    }
    return {detections_dir / file, "detections"};
}

} // namespace ttrally::cutting
