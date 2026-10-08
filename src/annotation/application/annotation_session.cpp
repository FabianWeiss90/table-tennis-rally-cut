// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/application/annotation_session.hpp"

namespace ttrally::annotation {

namespace {

ReviewPlan load_plan(ReviewItemSource& items, ReviewStateStore& states,
                     const std::string& video_id) {
    std::vector<ReviewItem> loaded = items.load();
    const ReviewStatusMap saved = states.load(video_id);
    for (auto& item : loaded) {
        const auto it = saved.find({item.kind, item.source_id});
        item.status = it != saved.end() ? it->second : ReviewStatus::Open;
    }
    return ReviewPlan(std::move(loaded));
}

AnnotationSheet load_sheet(const SessionSetup& setup, AnnotationRepository& annotations) {
    AnnotationSheet sheet(setup.video_id, setup.fps, setup.frame_count);
    for (auto& rally : annotations.load(setup.video_id)) {
        sheet.add(std::move(rally));
    }
    return sheet;
}

} // namespace

AnnotationSession::AnnotationSession(const SessionSetup& setup, AnnotationRepository& annotations,
                                     ReviewItemSource& items, ReviewStateStore& states)
    : video_id_(setup.video_id), annotations_(annotations), states_(states),
      sheet_(load_sheet(setup, annotations)), plan_(load_plan(items, states, setup.video_id)) {
    plan_.refresh(sheet_);
    save_states(); // also moves progress saved in an older format to the current one
    current_ = plan_.first_open();
    if (!current_ && !plan_.empty()) {
        current_ = 0;
    }
}

void AnnotationSession::select_item(std::size_t index) {
    static_cast<void>(plan_.item(index)); // bounds check
    current_ = index;
}

bool AnnotationSession::select_next_open() {
    const auto next = current_ ? plan_.next_open(*current_) : plan_.first_open();
    if (next) {
        current_ = next;
    }
    return next.has_value();
}

bool AnnotationSession::select_previous_open() {
    const auto previous = plan_.previous_open(current_.value_or(plan_.items().size()));
    if (previous) {
        current_ = previous;
    }
    return previous.has_value();
}

bool AnnotationSession::select_next_open_gap() {
    auto next = plan_.next_open(current_, ReviewKind::Gap);
    if (!next) {
        next = plan_.next_open(std::nullopt, ReviewKind::Gap); // wrap around
    }
    if (next) {
        current_ = next;
    }
    return next.has_value();
}

bool AnnotationSession::select_next_open_candidate() {
    const auto next = plan_.next_open(current_, ReviewKind::Candidate);
    if (next) {
        current_ = next;
    }
    return next.has_value();
}

void AnnotationSession::mark_start(std::int64_t frame) { draft_.start_frame = frame; }
void AnnotationSession::mark_end(std::int64_t frame) { draft_.end_frame = frame; }
void AnnotationSession::mark_serve_contact(std::int64_t frame) {
    draft_.serve_contact_frame = frame;
}
void AnnotationSession::clear_serve_contact() { draft_.serve_contact_frame.reset(); }
void AnnotationSession::set_aborted_toss(bool aborted) { draft_.aborted_toss = aborted; }
void AnnotationSession::set_let(bool let) { draft_.let = let; }
void AnnotationSession::set_notes(std::string notes) { draft_.notes = std::move(notes); }
void AnnotationSession::discard_draft() { draft_ = RallyDraft{}; }

void AnnotationSession::edit_rally(int id) {
    const RallyLabel& rally = sheet_.rally(id);
    draft_ = RallyDraft{id,           rally.start_frame,  rally.end_frame,
                        rally.serve_contact_frame, rally.aborted_toss, rally.notes, rally.let};
}

int AnnotationSession::save_draft() {
    if (!draft_.complete()) {
        throw AnnotationRuleViolation("Mark the start and the end of the rally first.");
    }
    RallyLabel rally{*draft_.start_frame, *draft_.end_frame, draft_.serve_contact_frame,
                     draft_.aborted_toss, draft_.notes, draft_.let};
    const int id = draft_.editing ? sheet_.replace(*draft_.editing, std::move(rally))
                                  : sheet_.add(std::move(rally));
    annotations_.save(sheet_);
    plan_.refresh(sheet_);
    save_states();
    draft_ = RallyDraft{};
    return id;
}

std::optional<int> AnnotationSession::save_draft_if_complete() {
    if (!draft_.complete()) {
        return std::nullopt;
    }
    return save_draft();
}

void AnnotationSession::delete_rally(int id) {
    sheet_.remove(id);
    annotations_.save(sheet_);
    plan_.refresh(sheet_);
    save_states();
    if (draft_.editing == id) {
        draft_ = RallyDraft{};
    } else if (draft_.editing && *draft_.editing > id) {
        draft_.editing = *draft_.editing - 1; // ids follow the order of the rallies
    }
}

std::optional<int> AnnotationSession::delete_rally_at(std::int64_t frame) {
    const auto id = sheet_.rally_at(frame);
    if (id) {
        delete_rally(*id);
    }
    return id;
}

void AnnotationSession::reject_current() { set_current_status(ReviewStatus::Rejected); }
void AnnotationSession::mark_current_reviewed() { set_current_status(ReviewStatus::Reviewed); }
void AnnotationSession::reopen_current() { set_current_status(ReviewStatus::Open); }

void AnnotationSession::set_current_status(ReviewStatus status) {
    if (!current_) {
        return;
    }
    const ReviewKind kind = plan_.item(*current_).kind;
    if (status == ReviewStatus::Rejected && kind != ReviewKind::Candidate) {
        throw AnnotationRuleViolation("X marks a segment without a rally; for a gap press R.");
    }
    if (status == ReviewStatus::Reviewed && kind != ReviewKind::Gap) {
        throw AnnotationRuleViolation("R marks a checked gap; for a segment without a rally "
                                      "press X.");
    }
    plan_.set_manual_status(*current_, status);
    plan_.refresh(sheet_);
    save_states();
}

void AnnotationSession::save_states() { states_.save(video_id_, plan_.items()); }

} // namespace ttrally::annotation
