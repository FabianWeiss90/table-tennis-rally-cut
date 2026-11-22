// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/white_balance_store.hpp"
#include "launcher/application/video_preview.hpp"
#include "launcher/domain/workspace.hpp"

#include <memory>
#include <string>

namespace ttrally::gui {

class GuiContext;

/// The white balance of a match day: per scene an area that should be neutral (dragged on the
/// scene's background), then per file (each part of a match on its own) the choice among "as
/// recorded", "neutral" and "like <another file of the scene>", each shown on the file's
/// background. Every change is saved at once; cut and highlights apply it.
class WhiteBalancePanel {
  public:
    WhiteBalancePanel(const GuiContext& gui, launcher::VideoPreview& preview,
                      cutting::WhiteBalanceStore& store);
    ~WhiteBalancePanel();
    WhiteBalancePanel(const WhiteBalancePanel&) = delete;
    WhiteBalancePanel& operator=(const WhiteBalancePanel&) = delete;

    /// Draws the panel for the match day; reads the files' backgrounds in the background first.
    void draw(const launcher::WorkspaceSnapshot& workspace, const std::string& day);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ttrally::gui
