// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"

#include <filesystem>

namespace ttrally::annotation {

/// Review progress in <directory>/<video_id>.review.csv (kind,id,status). Local working data,
/// not part of the versioned labels.
class CsvReviewStateStore final : public ReviewStateStore {
  public:
    explicit CsvReviewStateStore(std::filesystem::path directory)
        : directory_(std::move(directory)) {}

    [[nodiscard]] ReviewStatusMap load(const std::string& video_id) override;
    void save(const std::string& video_id, const ReviewStatusMap& statuses) override;

  private:
    [[nodiscard]] std::filesystem::path file_for(const std::string& video_id) const;

    std::filesystem::path directory_;
};

} // namespace ttrally::annotation
