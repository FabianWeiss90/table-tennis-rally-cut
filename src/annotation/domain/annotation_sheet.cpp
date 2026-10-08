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

namespace {

/// Inserts an entry keeping the list sorted by start; returns its 1-based position.
template <typename Entry> int insert_sorted(std::vector<Entry>& entries, Entry entry) {
    const auto it = std::upper_bound(
        entries.begin(), entries.end(), entry.start_frame,
        [](std::int64_t start, const Entry& other) { return start < other.start_frame; });
    const auto inserted = entries.insert(it, std::move(entry));
    return static_cast<int>(inserted - entries.begin()) + 1;
}

template <typename Entry>
std::size_t position_of(const std::vector<Entry>& entries, int id, std::string_view what) {
    if (id < 1 || static_cast<std::size_t>(id) > entries.size()) {
        throw std::out_of_range(std::format("there is no {} {}", what, id));
    }
    return static_cast<std::size_t>(id) - 1;
}

template <typename Entry> std::optional<int> id_at(const std::vector<Entry>& entries,
                                                  std::int64_t frame) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].contains(frame)) {
            return static_cast<int>(i) + 1;
        }
    }
    return std::nullopt;
}

template <typename Entry>
std::vector<int> ids_overlapping(const std::vector<Entry>& entries, std::int64_t first,
                                 std::int64_t last) {
    std::vector<int> ids;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].overlaps(first, last)) {
            ids.push_back(static_cast<int>(i) + 1);
        }
    }
    return ids;
}

constexpr std::string_view kRally = "rally";
constexpr std::string_view kIgnoredSection = "ignored section";

} // namespace

const RallyLabel& AnnotationSheet::rally(int id) const {
    return rallies_[position_of(rallies_, id, kRally)];
}

std::size_t AnnotationSheet::let_count() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(rallies_, [](const RallyLabel& rally) { return rally.let; }));
}

int AnnotationSheet::add(RallyLabel rally) {
    validate(rally, Skip{});
    return insert_sorted(rallies_, std::move(rally));
}

int AnnotationSheet::replace(int id, RallyLabel rally) {
    const std::size_t index = position_of(rallies_, id, kRally);
    validate(rally, Skip{.rally = index});
    rallies_.erase(rallies_.begin() + static_cast<std::ptrdiff_t>(index));
    return insert_sorted(rallies_, std::move(rally));
}

void AnnotationSheet::remove(int id) {
    rallies_.erase(rallies_.begin() +
                   static_cast<std::ptrdiff_t>(position_of(rallies_, id, kRally)));
}

std::optional<int> AnnotationSheet::rally_at(std::int64_t frame) const {
    return id_at(rallies_, frame);
}

std::vector<int> AnnotationSheet::rallies_overlapping(std::int64_t first, std::int64_t last) const {
    return ids_overlapping(rallies_, first, last);
}

const IgnoredSection& AnnotationSheet::ignored_section(int id) const {
    return ignored_[position_of(ignored_, id, kIgnoredSection)];
}

int AnnotationSheet::add_ignored(IgnoredSection section) {
    validate(section, Skip{});
    return insert_sorted(ignored_, std::move(section));
}

int AnnotationSheet::replace_ignored(int id, IgnoredSection section) {
    const std::size_t index = position_of(ignored_, id, kIgnoredSection);
    validate(section, Skip{.ignored = index});
    ignored_.erase(ignored_.begin() + static_cast<std::ptrdiff_t>(index));
    return insert_sorted(ignored_, std::move(section));
}

void AnnotationSheet::remove_ignored(int id) {
    ignored_.erase(ignored_.begin() +
                   static_cast<std::ptrdiff_t>(position_of(ignored_, id, kIgnoredSection)));
}

std::optional<int> AnnotationSheet::ignored_at(std::int64_t frame) const {
    return id_at(ignored_, frame);
}

std::vector<int> AnnotationSheet::ignored_overlapping(std::int64_t first, std::int64_t last) const {
    return ids_overlapping(ignored_, first, last);
}

void AnnotationSheet::validate(const RallyLabel& rally, Skip skip) const {
    validate_span(rally.start_frame, rally.end_frame, rally.notes, kRally, skip);
    if (rally.serve_contact_frame && !rally.contains(*rally.serve_contact_frame)) {
        throw AnnotationRuleViolation("The serve contact must lie inside the rally.");
    }
    if (rally.aborted_toss && rally.let) {
        throw AnnotationRuleViolation("A rally cannot be both an aborted toss and a let.");
    }
}

void AnnotationSheet::validate(const IgnoredSection& section, Skip skip) const {
    validate_span(section.start_frame, section.end_frame, section.notes, kIgnoredSection, skip);
}

void AnnotationSheet::validate_span(std::int64_t start, std::int64_t end,
                                    const std::string& notes, std::string_view what,
                                    Skip skip) const {
    if (start < 0 || end >= frame_count_) {
        throw AnnotationRuleViolation(std::format("The {} must lie inside the video (frames 0 to {}).",
                                                  what, frame_count_ - 1));
    }
    if (end < start) {
        throw AnnotationRuleViolation("The end frame must not be before the start frame.");
    }
    if (notes.find_first_of("\r\n") != std::string::npos) {
        throw AnnotationRuleViolation("Notes must not contain line breaks.");
    }
    for (std::size_t i = 0; i < rallies_.size(); ++i) {
        if (i != skip.rally && rallies_[i].overlaps(start, end)) {
            throw AnnotationRuleViolation(std::format(
                "The {} overlaps rally {} (frames {} to {}).", what, i + 1,
                rallies_[i].start_frame, rallies_[i].end_frame));
        }
    }
    for (std::size_t i = 0; i < ignored_.size(); ++i) {
        if (i != skip.ignored && ignored_[i].overlaps(start, end)) {
            throw AnnotationRuleViolation(std::format(
                "The {} overlaps ignored section {} (frames {} to {}).", what, i + 1,
                ignored_[i].start_frame, ignored_[i].end_frame));
        }
    }
}

} // namespace ttrally::annotation
