// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::launcher {

/// Folder of the part lists, inside a match day's work folder.
inline constexpr std::string_view kMatchesFolder = "matches";

/// A match recorded in several files of a match day. Every step treats it as one video with one
/// continuous timeline: its parts are read one after another through a part list (no copy, no
/// re-encoding).
struct Match {
    std::string id;                 ///< the match's video id
    std::vector<std::string> parts; ///< file names in the match day folder, recording order

    friend bool operator==(const Match&, const Match&) = default;
};

/// File name of a match's part list, e.g. "kaether-baumgarten.ffconcat".
[[nodiscard]] std::string part_list_name(std::string_view id);
/// File name of the list of its parts' pre-cut versions (e.g. from Liimba), aligned against it.
[[nodiscard]] std::string pre_cut_list_name(std::string_view id);

/// The id proposed for a match of these parts (their video ids, in order): what their ids have in
/// common at the start, without a trailing number or separator ("kaether-baumgarten-1" and
/// "-2" -> "kaether-baumgarten"); the first part's id if they have too little in common. A
/// number is added if the id is taken.
[[nodiscard]] std::string propose_match_id(std::span<const std::string> part_ids,
                                           std::span<const std::string> taken);

/// Whether an id can name a match: letters, digits, "-" and "_", not empty.
[[nodiscard]] bool is_valid_match_id(std::string_view id);

} // namespace ttrally::launcher
