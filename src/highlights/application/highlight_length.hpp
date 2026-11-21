// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/cut_plan.hpp"
#include "highlights/domain/highlight_selection.hpp"

#include <span>
#include <vector>

namespace ttrally::highlights {

/// Which marks a highlight video takes.
enum class HighlightVideo {
    Game,     ///< the game highlights of one game
    MatchDay, ///< the match day highlights of all games
};

/// Seconds of the clips a game adds to a highlight video: its marked rallies with the margins,
/// merged where they overlap (as `ttrally highlights` cuts them). The end of the video is not
/// known here, so a clip at its very end may come out a little shorter.
[[nodiscard]] std::vector<double> clip_lengths(const std::vector<HighlightRally>& rallies,
                                               HighlightVideo video, cutting::Margins margins,
                                               double fps);

/// Seconds of a video of these clips, each fading into the next for `crossfade_s` (at most half
/// of either clip, as the renderer does).
[[nodiscard]] double joined_length(std::span<const double> clips, double crossfade_s);

} // namespace ttrally::highlights
