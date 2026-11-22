// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttrally::highlights {

/// A rally of a highlight video: its game (video id) and its first frame identify it.
struct RallyKey {
    std::string game;
    std::int64_t start_frame = 0;

    friend bool operator==(const RallyKey&, const RallyKey&) = default;
};

/// The rallies of `chronological` (the default order: games by recording time, each game's
/// rallies by time) in the order chosen by the user (`custom`): those in it keep its order,
/// the others come right after the rally before them in the default order (first if none is).
/// Entries of `custom` that are no longer marked are dropped.
[[nodiscard]] std::vector<RallyKey> apply_order(const std::vector<RallyKey>& chronological,
                                                const std::vector<RallyKey>& custom);

/// Moves the entry at `from` to position `to` (both indices into `order`).
void move_entry(std::vector<RallyKey>& order, std::size_t from, std::size_t to);

} // namespace ttrally::highlights
