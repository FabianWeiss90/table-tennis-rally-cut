// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/white_balance_store.hpp"
#include "highlights/application/game_highlights.hpp"
#include "launcher/application/video_preview.hpp"
#include "launcher/domain/workspace.hpp"

#include <memory>
#include <string>

namespace ttrally::gui {

class GuiContext;

/// The white balance of a match day: in a picture from the middle of every file (each part of a
/// match on its own) a rectangle over the same spot, and a reference rally tuned with
/// temperature, tint and brightness, towards which cut and highlights correct every rally
/// (measured on its own frames). Every change is saved at once.
class WhiteBalancePanel {
  public:
    /// `days` lists the rallies to choose the reference from (nullptr: none offered).
    WhiteBalancePanel(const GuiContext& gui, launcher::VideoPreview& preview,
                      cutting::WhiteBalanceStore& store, highlights::MatchDayHighlights* days);
    ~WhiteBalancePanel();
    WhiteBalancePanel(const WhiteBalancePanel&) = delete;
    WhiteBalancePanel& operator=(const WhiteBalancePanel&) = delete;

    /// Draws the panel for the match day; reads the files' pictures in the background first.
    void draw(const launcher::WorkspaceSnapshot& workspace, const std::string& day);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttrally::gui
