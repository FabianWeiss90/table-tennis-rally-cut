// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/domain/highlight_selection.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace ttrally::highlights {

namespace {

std::int64_t shared_frames(const HighlightRally& a, const HighlightRally& b) {
    return std::max<std::int64_t>(
        0, std::min(a.end_frame, b.end_frame) - std::max(a.start_frame, b.start_frame) + 1);
}

} // namespace

HighlightSelection::HighlightSelection(std::vector<HighlightRally> rallies, bool coupled)
    : rallies_(std::move(rallies)), coupled_(coupled) {
    std::ranges::sort(rallies_, {}, &HighlightRally::start_frame);
    set_coupled(coupled);
}

void HighlightSelection::set_coupled(bool coupled) {
    coupled_ = coupled;
    if (coupled_) {
        for (HighlightRally& rally : rallies_) {
            rally.game = rally.game || rally.day;
        }
    }
}

void HighlightSelection::set_game(std::size_t index, bool marked) {
    HighlightRally& rally = rallies_.at(index);
    rally.game = marked;
    if (!marked && coupled_) {
        rally.day = false;
    }
}

void HighlightSelection::set_slow_motion(std::size_t index, std::optional<SlowMotion> slow) {
    rallies_.at(index).slow = slow;
}

void HighlightSelection::set_day(std::size_t index, bool marked) {
    HighlightRally& rally = rallies_.at(index);
    rally.day = marked;
    if (marked && coupled_) {
        rally.game = true;
    }
}

std::vector<std::size_t> HighlightSelection::by_length() const {
    std::vector<std::size_t> order(rallies_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    // Rallies are sorted by start, so a stable sort keeps the earlier of equal lengths first
    std::ranges::stable_sort(order, std::greater{},
                             [this](std::size_t i) { return rallies_[i].frame_count(); });
    return order;
}

void HighlightSelection::preselect(std::size_t count) {
    const std::vector<std::size_t> order = by_length();
    clear();
    for (std::size_t i = 0; i < std::min(count, order.size()); ++i) {
        rallies_[order[i]].game = true;
        rallies_[order[i]].day = true;
    }
}

void HighlightSelection::mark_longest_for_day(std::size_t count) {
    const std::vector<std::size_t> order = by_length();
    for (std::size_t i = 0; i < order.size(); ++i) {
        set_day(order[i], i < count);
    }
}

void HighlightSelection::clear() {
    for (HighlightRally& rally : rallies_) {
        rally.game = false;
        rally.day = false;
    }
}

std::size_t HighlightSelection::game_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(rallies_, true, &HighlightRally::game));
}

std::size_t HighlightSelection::day_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(rallies_, true, &HighlightRally::day));
}

std::vector<HighlightRally> carry_over_marks(std::vector<HighlightRally> rallies,
                                             const std::vector<HighlightRally>& saved) {
    for (HighlightRally& rally : rallies) {
        const HighlightRally* best = nullptr;
        std::int64_t best_shared = 0;
        for (const HighlightRally& old : saved) {
            const std::int64_t shared = shared_frames(rally, old);
            if (shared > best_shared) {
                best = &old;
                best_shared = shared;
            }
        }
        if (best != nullptr &&
            2 * best_shared >= std::min(rally.frame_count(), best->frame_count())) {
            rally.game = best->game;
            rally.day = best->day;
            // A slow motion stays if it still lies in the rally (with a margin of a second
            // or so either side, which the frames cannot tell; the rally itself is the limit)
            if (best->slow && best->slow->end_frame > rally.start_frame &&
                best->slow->start_frame < rally.end_frame) {
                rally.slow = best->slow;
            }
        } else {
            rally.game = false;
            rally.day = false;
        }
    }
    return rallies;
}

} // namespace ttrally::highlights
