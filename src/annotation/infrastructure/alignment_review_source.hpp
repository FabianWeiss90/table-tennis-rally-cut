// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"

#include <filesystem>

namespace ttrally::annotation {

/// Review items from the CSV files written by `ttrally align`: segments become candidates,
/// gaps (if the gaps file exists) become gaps.
class AlignmentReviewSource final : public ReviewItemSource {
  public:
    AlignmentReviewSource(std::filesystem::path segments_csv, std::filesystem::path gaps_csv)
        : segments_csv_(std::move(segments_csv)), gaps_csv_(std::move(gaps_csv)) {}

    [[nodiscard]] std::vector<ReviewItem> load() override;

  private:
    std::filesystem::path segments_csv_;
    std::filesystem::path gaps_csv_;
};

} // namespace ttrally::annotation
