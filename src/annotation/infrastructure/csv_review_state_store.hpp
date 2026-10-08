// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"

#include <filesystem>
#include <optional>

namespace ttrally::annotation {

/// Review progress in <directory>/<video_id>.review.csv, next to the labels: kind,id,
/// first_frame,last_frame,status for every candidate and gap. Training uses it to
/// check that a video was reviewed completely. Files of the older local format (kind,id,status
/// in `legacy_directory`) are read if no current file exists.
class CsvReviewStateStore final : public ReviewStateStore {
  public:
    explicit CsvReviewStateStore(std::filesystem::path directory,
                                 std::optional<std::filesystem::path> legacy_directory = {})
        : directory_(std::move(directory)), legacy_directory_(std::move(legacy_directory)) {}

    [[nodiscard]] ReviewStatusMap load(const std::string& video_id) override;
    void save(const std::string& video_id, const std::vector<ReviewItem>& items) override;

    [[nodiscard]] std::filesystem::path file_for(const std::string& video_id) const;

  private:
    std::filesystem::path directory_;
    std::optional<std::filesystem::path> legacy_directory_;
};

} // namespace ttrally::annotation
