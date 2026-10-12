// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>

namespace ttrally::cli {

/// `ttrally gui`: a window to run all commands with forms filled in for a video.
class GuiCommand {
  public:
    explicit GuiCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::filesystem::path project_ = ".";
};

} // namespace ttrally::cli
