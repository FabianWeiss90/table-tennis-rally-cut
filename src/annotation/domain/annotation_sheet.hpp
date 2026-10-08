// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/domain/ignored_section.hpp"
#include "annotation/domain/rally_label.hpp"
#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::annotation {

/// A change would break an annotation rule (e.g. overlapping rallies).
class AnnotationRuleViolation : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Aggregate root: all rallies and ignored sections annotated in one video.
///
/// Invariants: every rally and ignored section lies inside the video, starts before or at its
/// end and has no line break in its notes; a rally's serve contact (if any) lies inside it; no
/// two of them overlap. Both are kept sorted by start; the id of a rally (or of an ignored
/// section) is its 1-based position among the rallies (or among the ignored sections).
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

    // Ignored sections (left out of training)
    [[nodiscard]] const std::vector<IgnoredSection>& ignored_sections() const noexcept {
        return ignored_;
    }
    [[nodiscard]] const IgnoredSection& ignored_section(int id) const;
    /// Adds an ignored section and returns its id. Throws AnnotationRuleViolation.
    int add_ignored(IgnoredSection section);
    /// Replaces an ignored section and returns its (possibly new) id. Throws
    /// AnnotationRuleViolation.
    int replace_ignored(int id, IgnoredSection section);
    void remove_ignored(int id);
    /// Id of the ignored section containing the frame, if any.
    [[nodiscard]] std::optional<int> ignored_at(std::int64_t frame) const;
    /// Ids of the ignored sections overlapping [first, last].
    [[nodiscard]] std::vector<int> ignored_overlapping(std::int64_t first,
                                                       std::int64_t last) const;

  private:
    /// What a change leaves out when checking for overlaps: the entry being replaced.
    struct Skip {
        std::optional<std::size_t> rally;
        std::optional<std::size_t> ignored;
    };
    void validate(const RallyLabel& rally, Skip skip) const;
    void validate(const IgnoredSection& section, Skip skip) const;
    /// Rules shared by rallies and ignored sections; `what` names the entry in messages.
    void validate_span(std::int64_t start, std::int64_t end, const std::string& notes,
                       std::string_view what, Skip skip) const;

    std::string video_id_;
    Rational fps_;
    std::int64_t frame_count_;
    std::vector<RallyLabel> rallies_;
    std::vector<IgnoredSection> ignored_;
};

} // namespace ttrally::annotation
