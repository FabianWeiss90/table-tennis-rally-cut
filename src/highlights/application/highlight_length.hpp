// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/cut_plan.hpp"
#include "highlights/domain/highlight_selection.hpp"

#include <span>
#include <string>
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

/// A marked rally in the order of a highlight video, with the frame rate of its game.
struct TimedRally {
    std::string game; ///< video id
    HighlightRally rally;
    double fps = 0.0;
};

/// Seconds of the clips of rallies in the order of a highlight video: rallies of one game that
/// follow each other rising in time share clips where their margins overlap (as `ttrally
/// highlights` cuts them).
[[nodiscard]] std::vector<double> ordered_clip_lengths(const std::vector<TimedRally>& rallies,
                                                       cutting::Margins margins);

/// Seconds of a video of these clips, each fading into the next for `crossfade_s` (at most half
/// of either clip, as the renderer does).
[[nodiscard]] double joined_length(std::span<const double> clips, double crossfade_s);

/// A game's marks with its frame rate, for fit_match_day.
struct GameMarks {
    HighlightSelection selection;
    double fps = 0.0;
};

/// How many rallies every game gives to a match day video that should last at most `max_s`.
struct DayFit {
    std::size_t per_game = 0; ///< the longest rallies of each game (a game with fewer: all)
    double length_s = 0.0;    ///< of the match day video then
};

/// The most rallies per game (the longest of each, the same number from every game) whose match
/// day video, with the margins and cross-fades, lasts at most `max_s`. Time left over is not
/// filled with further rallies of single games, so every game has as many as the others (unless
/// it has fewer rallies at all).
[[nodiscard]] DayFit fit_match_day(std::span<const GameMarks> games, double max_s,
                                   cutting::Margins margins, double crossfade_s);

} // namespace ttrally::highlights
