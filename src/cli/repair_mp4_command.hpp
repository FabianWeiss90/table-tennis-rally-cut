// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <vector>

namespace ttrally::cli {

/// `ttrally repair-mp4`: repairs the video header of MP4 files written by earlier versions, so
/// that Apple's players play them (no re-encoding).
class RepairMp4Command {
  public:
    explicit RepairMp4Command(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::vector<std::filesystem::path> paths_;
    bool check_only_ = false;
};

} // namespace ttrally::cli
