// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/ports.hpp"
#include "annotation/domain/annotation_sheet.hpp"
#include "annotation/domain/review_plan.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace ttrally::annotation {

/// The rally currently being marked: a new one or an edited copy of a saved one.
struct RallyDraft {
    std::optional<int> editing; ///< Id of the saved rally being edited
    std::optional<std::int64_t> start_frame;
    std::optional<std::int64_t> end_frame;
    std::optional<std::int64_t> serve_contact_frame;
    bool aborted_toss = false;
    std::string notes;
    bool let = false;

    [[nodiscard]] bool complete() const noexcept { return start_frame && end_frame; }
    [[nodiscard]] bool empty() const noexcept {
        return !editing && !start_frame && !end_frame && !serve_contact_frame && !aborted_toss &&
               !let && notes.empty();
    }
};

struct SessionSetup {
    std::string video_id;
    Rational fps;
    std::int64_t frame_count = 0;
};

/// Application service behind the annotation GUI: walks through candidates and gaps, keeps the
/// rally draft and saves every change immediately through the repositories.
class AnnotationSession {
  public:
    /// Loads saved rallies, review items and review state. Throws AnnotationRuleViolation if the
    /// saved rallies break a rule.
    AnnotationSession(const SessionSetup& setup, AnnotationRepository& annotations,
                      ReviewItemSource& items, ReviewStateStore& states);

    [[nodiscard]] const AnnotationSheet& sheet() const noexcept { return sheet_; }
    [[nodiscard]] const ReviewPlan& plan() const noexcept { return plan_; }
    [[nodiscard]] const RallyDraft& draft() const noexcept { return draft_; }
    [[nodiscard]] std::optional<std::size_t> current_item() const noexcept { return current_; }

    // Navigation through the review items
    void select_item(std::size_t index);
    bool select_next_open();
    bool select_previous_open();
    /// Selects the next segment that has no rally yet (gaps are skipped).
    bool select_next_open_candidate();
    /// Selects the next gap that is not done yet, continuing from the start if there is none
    /// after the current item (segments are skipped).
    bool select_next_open_gap();

    // Rally draft
    void mark_start(std::int64_t frame);
    void mark_end(std::int64_t frame);
    void mark_serve_contact(std::int64_t frame);
    void clear_serve_contact();
    void set_aborted_toss(bool aborted);
    void set_let(bool let);
    void set_notes(std::string notes);
    void discard_draft();
    /// Loads a saved rally into the draft for editing.
    void edit_rally(int id);

    /// Saves the draft as a new or edited rally and returns its id. Throws
    /// AnnotationRuleViolation if the draft is incomplete or breaks a rule.
    int save_draft();
    /// Saves the draft if start and end are marked (nullopt otherwise; the draft is kept).
    /// Throws AnnotationRuleViolation if the draft breaks a rule.
    std::optional<int> save_draft_if_complete();
    void delete_rally(int id);

    // Review status of the current item
    void reject_current();         ///< Candidate without a rally
    void mark_current_reviewed();  ///< Gap without a rally
    void reopen_current();

    /// Segments that neither contain a rally nor were rejected (checked before closing).
    [[nodiscard]] std::vector<std::size_t> segments_without_rally() const {
        return plan_.open_items(ReviewKind::Candidate);
    }
    /// Gaps neither containing a rally nor marked as checked.
    [[nodiscard]] std::vector<std::size_t> unchecked_gaps() const {
        return plan_.open_items(ReviewKind::Gap);
    }
    /// Deletes the saved rally containing the frame; returns its id, nullopt if there is none.
    std::optional<int> delete_rally_at(std::int64_t frame);

  private:
    void set_current_status(ReviewStatus status);
    void save_states();

    std::string video_id_;
    AnnotationRepository& annotations_;
    ReviewStateStore& states_;
    AnnotationSheet sheet_;
    ReviewPlan plan_;
    RallyDraft draft_;
    std::optional<std::size_t> current_;
};

} // namespace ttrally::annotation
