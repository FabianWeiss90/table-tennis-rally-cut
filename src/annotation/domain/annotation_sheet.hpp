// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/domain/rally_label.hpp"
#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ttrally::annotation {

/// A change would break an annotation rule (e.g. overlapping rallies).
class AnnotationRuleViolation : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Aggregate root: all rallies annotated in one video.
///
/// Invariants: every rally lies inside the video, starts before or at its end, has its serve
/// contact (if any) inside the rally, no line break in its notes, and no two rallies overlap.
/// Rallies are kept sorted by start; a rally's id is its 1-based position.
class AnnotationSheet {
  public:
    AnnotationSheet(std::string video_id, Rational fps, std::int64_t frame_count);

    [[nodiscard]] const std::string& video_id() const noexcept { return video_id_; }
    [[nodiscard]] Rational fps() const noexcept { return fps_; }
    [[nodiscard]] std::int64_t frame_count() const noexcept { return frame_count_; }
    [[nodiscard]] const std::vector<RallyLabel>& rallies() const noexcept { return rallies_; }
    [[nodiscard]] const RallyLabel& rally(int id) const;
    /// Number of rallies flagged as let.
    [[nodiscard]] std::size_t let_count() const;

    /// Adds a rally and returns its id. Throws AnnotationRuleViolation.
    int add(RallyLabel rally);
    /// Replaces a rally and returns its (possibly new) id. Throws AnnotationRuleViolation.
    int replace(int id, RallyLabel rally);
    void remove(int id);

    /// Id of the rally containing the frame, if any.
    [[nodiscard]] std::optional<int> rally_at(std::int64_t frame) const;
    /// Ids of the rallies overlapping [first, last].
    [[nodiscard]] std::vector<int> rallies_overlapping(std::int64_t first,
                                                       std::int64_t last) const;

  private:
    void validate(const RallyLabel& rally, std::optional<std::size_t> ignored) const;
    int insert_sorted(RallyLabel rally);
    [[nodiscard]] std::size_t position(int id) const;

    std::string video_id_;
    Rational fps_;
    std::int64_t frame_count_;
    std::vector<RallyLabel> rallies_;
};

} // namespace ttrally::annotation
