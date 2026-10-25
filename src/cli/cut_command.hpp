// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>

namespace ttrally::cli {

/// `ttrally cut`: cuts a video down to its rallies, with a margin before and after each.
class CutCommand {
  public:
    explicit CutCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::filesystem::path video_;
    std::string video_id_;
    std::filesystem::path rallies_;
    std::filesystem::path out_;
    std::filesystem::path annotations_dir_ = "data/annotations";
    std::filesystem::path detections_dir_ = "data/detections";
    std::filesystem::path cache_dir_ = "data/cache";
    double before_s_ = 1.0;
    double after_s_ = 1.5;
    bool fast_ = false;
    std::string codec_ = "h265";
    std::string encoder_ = "auto";
    double bitrate_mbps_ = 0.0;
};

} // namespace ttrally::cli
