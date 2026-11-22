// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/cut_plan.hpp"
#include "highlights/application/game_highlights.hpp"
#include "highlights/application/playback_source.hpp"
#include "highlights/application/title_fonts.hpp"
#include "launcher/domain/workspace.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace ttrally::gui {

class GuiContext;

/// What the panel offers to make.
struct HighlightCounts {
    bool game_loaded = false;      ///< the selected video has rallies to choose from
    std::size_t game = 0;          ///< game highlights of the selected video
    std::size_t match_day = 0;     ///< match day highlights of all games of its match day
};

/// The highlights part of the launcher: the rallies of the selected video's game (or of every
/// game of a match day chosen as a whole), numbered, with
/// a mark for the game and one for the match day, the preselection by length, a player to watch
/// a rally and the title. Every change is saved at once in the match day's highlights folder.
class HighlightsPanel {
  public:
    HighlightsPanel(const GuiContext& gui, highlights::MatchDayHighlights& days,
                    highlights::PlaybackSource& playback, highlights::TitleFonts& fonts);
    ~HighlightsPanel();
    HighlightsPanel(const HighlightsPanel&) = delete;
    HighlightsPanel& operator=(const HighlightsPanel&) = delete;

    /// Draws the panel for the video; `reload` reads the rallies and marks again (e.g. when the
    /// tab is opened). `margins` are played around a rally; with `crossfade_s` they give the
    /// lengths of the highlight videos shown.
    void draw(const launcher::WorkspaceSnapshot& workspace, const launcher::VideoEntry& video,
              bool reload, cutting::Margins margins, double crossfade_s);
    /// Draws the match day as a whole: the rallies of all its games in one list, under a row
    /// per game, with both marks, the player, the match title and the match day layout.
    void draw_match_day(const launcher::WorkspaceSnapshot& workspace,
                        const std::string& match_day, bool reload, cutting::Margins margins,
                        double crossfade_s);

    [[nodiscard]] HighlightCounts counts() const;

    /// The visible height of the tab below the player in the last drawn frame (negative: that
    /// much of the player is hidden). Moving the output down by it makes the rally list exactly
    /// as high as the player.
    [[nodiscard]] float spare_height() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttrally::gui
