// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/white_balance_store.hpp"
#include "launcher/application/video_preview.hpp"
#include "launcher/domain/workspace.hpp"

#include <memory>
#include <string>

namespace ttrally::gui {

class GuiContext;

/// The white balance of a match day: in a picture from the middle of every file (each part of a
/// match on its own) a rectangle over the same spot, one file tuned with temperature and tint as
/// the reference, and every other file corrected so that its spot looks like the reference's.
/// Every change is saved at once; cut and highlights apply it.
class WhiteBalancePanel {
  public:
    WhiteBalancePanel(const GuiContext& gui, launcher::VideoPreview& preview,
                      cutting::WhiteBalanceStore& store);
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
