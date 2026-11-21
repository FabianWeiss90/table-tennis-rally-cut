// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <vector>

namespace ttrally::cli {

/// `ttrally merge`: a game recorded in several files as one video, without re-encoding.
class MergeCommand {
  public:
    explicit MergeCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::vector<std::filesystem::path> parts_;
    std::filesystem::path out_;
};

} // namespace ttrally::cli
