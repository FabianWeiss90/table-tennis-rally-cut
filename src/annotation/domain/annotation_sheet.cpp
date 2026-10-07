// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/domain/annotation_sheet.hpp"

#include <algorithm>
#include <format>

namespace ttrally::annotation {

AnnotationSheet::AnnotationSheet(std::string video_id, Rational fps, std::int64_t frame_count)
    : video_id_(std::move(video_id)), fps_(fps), frame_count_(frame_count) {
    if (video_id_.empty() || !fps_.positive() || frame_count_ <= 0) {
        throw std::invalid_argument(
            "an annotation sheet needs a video id, a frame rate and frames");
    }
}

const RallyLabel& AnnotationSheet::rally(int id) const { return rallies_[position(id)]; }

int AnnotationSheet::add(RallyLabel rally) {
    validate(rally, std::nullopt);
    return insert_sorted(std::move(rally));
}

int AnnotationSheet::replace(int id, RallyLabel rally) {
    const std::size_t index = position(id);
    validate(rally, index);
    rallies_.erase(rallies_.begin() + static_cast<std::ptrdiff_t>(index));
    return insert_sorted(std::move(rally));
}

void AnnotationSheet::remove(int id) {
    rallies_.erase(rallies_.begin() + static_cast<std::ptrdiff_t>(position(id)));
}

std::optional<int> AnnotationSheet::rally_at(std::int64_t frame) const {
    for (std::size_t i = 0; i < rallies_.size(); ++i) {
        if (rallies_[i].contains(frame)) {
            return static_cast<int>(i) + 1;
        }
    }
    return std::nullopt;
}

std::vector<int> AnnotationSheet::rallies_overlapping(std::int64_t first, std::int64_t last) const {
    std::vector<int> ids;
    for (std::size_t i = 0; i < rallies_.size(); ++i) {
        if (rallies_[i].overlaps(first, last)) {
            ids.push_back(static_cast<int>(i) + 1);
        }
    }
    return ids;
}

void AnnotationSheet::validate(const RallyLabel& rally, std::optional<std::size_t> ignored) const {
    if (rally.start_frame < 0 || rally.end_frame >= frame_count_) {
        throw AnnotationRuleViolation(std::format(
            "The rally must lie inside the video (frames 0 to {}).", frame_count_ - 1));
    }
    if (rally.end_frame < rally.start_frame) {
        throw AnnotationRuleViolation("The end frame must not be before the start frame.");
    }
    if (rally.serve_contact_frame && !rally.contains(*rally.serve_contact_frame)) {
        throw AnnotationRuleViolation("The serve contact must lie inside the rally.");
    }
    if (rally.aborted_toss && rally.let) {
        throw AnnotationRuleViolation("A rally cannot be both an aborted toss and a let.");
    }
    if (rally.notes.find_first_of("\r\n") != std::string::npos) {
        throw AnnotationRuleViolation("Notes must not contain line breaks.");
    }
    for (std::size_t i = 0; i < rallies_.size(); ++i) {
        if (i != ignored && rallies_[i].overlaps(rally.start_frame, rally.end_frame)) {
            throw AnnotationRuleViolation(std::format(
                "The rally overlaps rally {} (frames {} to {}).", i + 1, rallies_[i].start_frame,
                rallies_[i].end_frame));
        }
    }
}

int AnnotationSheet::insert_sorted(RallyLabel rally) {
    const auto it = std::upper_bound(
        rallies_.begin(), rallies_.end(), rally.start_frame,
        [](std::int64_t start, const RallyLabel& other) { return start < other.start_frame; });
    const auto inserted = rallies_.insert(it, std::move(rally));
    return static_cast<int>(inserted - rallies_.begin()) + 1;
}

std::size_t AnnotationSheet::position(int id) const {
    if (id < 1 || static_cast<std::size_t>(id) > rallies_.size()) {
        throw std::out_of_range(std::format("there is no rally {}", id));
    }
    return static_cast<std::size_t>(id) - 1;
}

} // namespace ttrally::annotation
