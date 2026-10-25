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

/// Open: not looked at yet. Annotated: a candidate overlapping a rally, or a gap completely
/// covered by rallies and ignored sections (derived from the sheet). Ignored: the same with
/// ignored sections only (derived from the sheet). Rejected: a candidate without a rally.
/// Reviewed: a gap checked by the user (with or without rallies found in it).
enum class ReviewStatus { Open, Annotated, Rejected, Reviewed, Ignored };

[[nodiscard]] std::string_view to_string(ReviewKind kind) noexcept;
[[nodiscard]] std::string_view to_string(ReviewStatus status) noexcept;
[[nodiscard]] std::optional<ReviewKind> parse_review_kind(std::string_view text) noexcept;
[[nodiscard]] std::optional<ReviewStatus> parse_review_status(std::string_view text) noexcept;

/// Source id of the gap that covers a whole video without alignment (alignment gaps start at 1).
inline constexpr int kWholeVideoGapId = 0;

struct ReviewItem {
    ReviewKind kind = ReviewKind::Candidate;
    int source_id = 0;              ///< segment_id or gap_id from the alignment output
    std::int64_t first_frame = 0;   ///< Hint from the alignment, not exact
    std::int64_t last_frame = 0;    ///< inclusive
    ReviewStatus status = ReviewStatus::Open;

    [[nodiscard]] std::string title() const;
    [[nodiscard]] bool done() const noexcept { return status != ReviewStatus::Open; }
};

/// One gap over all frames, for a video without a pre-cut version: every rally is marked by
/// hand, and the gap is checked (Reviewed) once the whole video has been looked at.
[[nodiscard]] ReviewItem whole_video_gap(std::int64_t frame_count);

struct ReviewProgress {
    std::size_t candidates = 0;
    std::size_t candidates_done = 0;
    std::size_t gaps = 0;
    std::size_t gaps_done = 0;

    /// Every candidate and gap was looked at: the video is ready for training.
    [[nodiscard]] bool complete() const noexcept {
        return candidates + gaps > 0 && candidates_done == candidates && gaps_done == gaps;
    }
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

    /// Recomputes the statuses: candidates overlapping a rally are Annotated, candidates
    /// overlapping only an ignored section are Ignored. Gaps get these statuses only if rallies
    /// and ignored sections cover them completely; a gap that is only partly covered may still
    /// hide a rally and keeps its manual status (Open until checked). The others keep their
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
