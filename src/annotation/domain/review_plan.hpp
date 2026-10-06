// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/domain/annotation_sheet.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::annotation {

/// What has to be looked at: a candidate segment found by `align`, or a gap between candidates
/// that may hide a missed rally.
enum class ReviewKind { Candidate, Gap };

/// Open: not looked at yet. Annotated: contains at least one rally (derived from the sheet).
/// Rejected: a candidate without a rally. Reviewed: a gap checked without finding a rally.
enum class ReviewStatus { Open, Annotated, Rejected, Reviewed };

[[nodiscard]] std::string_view to_string(ReviewKind kind) noexcept;
[[nodiscard]] std::string_view to_string(ReviewStatus status) noexcept;
[[nodiscard]] std::optional<ReviewKind> parse_review_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<ReviewStatus> parse_review_status(std::string_view text) noexcept;

struct ReviewItem {
    ReviewKind kind = ReviewKind::Candidate;
    int source_id = 0;              ///< segment_id or gap_id from the alignment output
    std::int64_t first_frame = 0;   ///< Hint from the alignment, not exact
    std::int64_t last_frame = 0;    ///< inclusive
    ReviewStatus status = ReviewStatus::Open;

    [[nodiscard]] std::string title() const;
    [[nodiscard]] bool done() const noexcept { return status != ReviewStatus::Open; }
};

struct ReviewProgress {
    std::size_t candidates = 0;
    std::size_t candidates_done = 0;
    std::size_t gaps = 0;
    std::size_t gaps_done = 0;
};

/// The list of candidates and gaps in video order, with their review status.
class ReviewPlan {
  public:
    explicit ReviewPlan(std::vector<ReviewItem> items);

    [[nodiscard]] const std::vector<ReviewItem>& items() const noexcept { return items_; }
    [[nodiscard]] const ReviewItem& item(std::size_t index) const { return items_.at(index); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    /// Sets the manual status of an item (Open, Rejected for candidates, Reviewed for gaps).
    /// Throws std::invalid_argument for statuses that do not fit the item.
    void set_manual_status(std::size_t index, ReviewStatus status);
    [[nodiscard]] ReviewStatus manual_status(std::size_t index) const;

    /// Recomputes the statuses: items overlapping a rally are Annotated, the others keep their
    /// manual status.
    void refresh(const AnnotationSheet& sheet);

    [[nodiscard]] std::optional<std::size_t> next_open(std::size_t after) const;
    /// Next open item of the given kind after `after` (or from the start if nullopt).
    [[nodiscard]] std::optional<std::size_t> next_open(std::optional<std::size_t> after,
                                                       ReviewKind kind) const;
    [[nodiscard]] std::optional<std::size_t> previous_open(std::size_t before) const;
    [[nodiscard]] std::optional<std::size_t> first_open() const;
    /// Indices of all open items of a kind (e.g. segments without a rally and not rejected).
    [[nodiscard]] std::vector<std::size_t> open_items(ReviewKind kind) const;
    [[nodiscard]] ReviewProgress progress() const;

  private:
    std::vector<ReviewItem> items_;
    std::vector<ReviewStatus> manual_;
};

} // namespace ttrally::annotation
