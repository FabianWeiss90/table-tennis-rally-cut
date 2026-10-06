// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/domain/review_plan.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>
#include <utility>

namespace ttrally::annotation {

namespace {

constexpr std::array<std::pair<ReviewKind, std::string_view>, 2> kKinds{
    {{ReviewKind::Candidate, "candidate"}, {ReviewKind::Gap, "gap"}}};
constexpr std::array<std::pair<ReviewStatus, std::string_view>, 4> kStatuses{
    {{ReviewStatus::Open, "open"},
     {ReviewStatus::Annotated, "annotated"},
     {ReviewStatus::Rejected, "rejected"},
     {ReviewStatus::Reviewed, "reviewed"}}};

template <typename Enum, std::size_t N>
std::string_view name_of(const std::array<std::pair<Enum, std::string_view>, N>& names,
                         Enum value) noexcept {
    for (const auto& [entry, name] : names) {
        if (entry == value) {
            return name;
        }
    }
    return "unknown";
}

template <typename Enum, std::size_t N>
std::optional<Enum> parse(const std::array<std::pair<Enum, std::string_view>, N>& names,
                          std::string_view text) noexcept {
    for (const auto& [entry, name] : names) {
        if (name == text) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view to_string(ReviewKind kind) noexcept { return name_of(kKinds, kind); }
std::string_view to_string(ReviewStatus status) noexcept { return name_of(kStatuses, status); }
std::optional<ReviewKind> parse_review_kind(std::string_view text) noexcept {
    return parse(kKinds, text);
}
std::optional<ReviewStatus> parse_review_status(std::string_view text) noexcept {
    return parse(kStatuses, text);
}

std::string ReviewItem::title() const {
    return std::format("{} {}", kind == ReviewKind::Candidate ? "Segment" : "Gap", source_id);
}

ReviewPlan::ReviewPlan(std::vector<ReviewItem> items) : items_(std::move(items)) {
    std::stable_sort(items_.begin(), items_.end(), [](const ReviewItem& a, const ReviewItem& b) {
        return a.first_frame < b.first_frame;
    });
    manual_.reserve(items_.size());
    for (const auto& item : items_) {
        manual_.push_back(item.status == ReviewStatus::Annotated ? ReviewStatus::Open
                                                                 : item.status);
    }
}

void ReviewPlan::set_manual_status(std::size_t index, ReviewStatus status) {
    const ReviewKind kind = items_.at(index).kind;
    const bool fits = status == ReviewStatus::Open ||
                      (status == ReviewStatus::Rejected && kind == ReviewKind::Candidate) ||
                      (status == ReviewStatus::Reviewed && kind == ReviewKind::Gap);
    if (!fits) {
        throw std::invalid_argument(std::format("status {} does not apply to a {}",
                                                to_string(status), to_string(kind)));
    }
    manual_[index] = status;
    if (items_[index].status != ReviewStatus::Annotated) {
        items_[index].status = status;
    }
}

ReviewStatus ReviewPlan::manual_status(std::size_t index) const { return manual_.at(index); }

void ReviewPlan::refresh(const AnnotationSheet& sheet) {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        auto& item = items_[i];
        const auto rallies = sheet.rallies_overlapping(item.first_frame, item.last_frame);
        item.status = !rallies.empty() ? ReviewStatus::Annotated : manual_[i];
    }
}

std::optional<std::size_t> ReviewPlan::next_open(std::size_t after) const {
    for (std::size_t i = after + 1; i < items_.size(); ++i) {
        if (!items_[i].done()) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> ReviewPlan::next_open(std::optional<std::size_t> after,
                                                  ReviewKind kind) const {
    for (std::size_t i = after ? *after + 1 : 0; i < items_.size(); ++i) {
        if (items_[i].kind == kind && !items_[i].done()) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> ReviewPlan::previous_open(std::size_t before) const {
    for (std::size_t i = std::min(before, items_.size()); i-- > 0;) {
        if (!items_[i].done()) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> ReviewPlan::first_open() const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].done()) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> ReviewPlan::open_items(ReviewKind kind) const {
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].kind == kind && !items_[i].done()) {
            open.push_back(i);
        }
    }
    return open;
}

ReviewProgress ReviewPlan::progress() const {
    ReviewProgress progress;
    for (const auto& item : items_) {
        const bool candidate = item.kind == ReviewKind::Candidate;
        (candidate ? progress.candidates : progress.gaps) += 1;
        if (item.done()) {
            (candidate ? progress.candidates_done : progress.gaps_done) += 1;
        }
    }
    return progress;
}

} // namespace ttrally::annotation
