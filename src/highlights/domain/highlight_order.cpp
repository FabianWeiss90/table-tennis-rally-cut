// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/domain/highlight_order.hpp"

#include <algorithm>
#include <iterator>

namespace ttrally::highlights {

std::vector<RallyKey> apply_order(const std::vector<RallyKey>& chronological,
                                  const std::vector<RallyKey>& custom) {
    std::vector<RallyKey> ordered;
    for (const RallyKey& key : custom) {
        if (std::ranges::find(chronological, key) != chronological.end() &&
            std::ranges::find(ordered, key) == ordered.end()) {
            ordered.push_back(key);
        }
    }
    for (std::size_t i = 0; i < chronological.size(); ++i) {
        const RallyKey& key = chronological[i];
        if (std::ranges::find(ordered, key) != ordered.end()) {
            continue;
        }
        // Right after the rally before it in the default order (it is placed already)
        auto at = ordered.begin();
        if (i > 0) {
            at = std::next(std::ranges::find(ordered, chronological[i - 1]));
        }
        ordered.insert(at, key);
    }
    return ordered;
}

void move_entry(std::vector<RallyKey>& order, std::size_t from, std::size_t to) {
    if (from >= order.size() || to >= order.size() || from == to) {
        return;
    }
    RallyKey moved = std::move(order[from]);
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(from));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
}

} // namespace ttrally::highlights
