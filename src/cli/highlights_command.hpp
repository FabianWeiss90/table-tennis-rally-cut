// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>

namespace ttrally::cli {

/// `ttrally highlights`: the highlight video of a game or of a match day, from the rallies
/// marked in the launcher.
class HighlightsCommand {
  public:
    explicit HighlightsCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::string video_id_;
    bool match_day_ = false;
    std::filesystem::path highlights_dir_ = "data/highlights";
    std::filesystem::path out_;
    std::filesystem::path cache_dir_ = "data/cache";
    double before_s_ = 1.0;
    double after_s_ = 1.5;
    double crossfade_s_ = 0.5;
    bool rally_titles_ = false;
    std::filesystem::path font_;
    std::filesystem::path shapes_dir_;
    std::string codec_ = "h265";
    std::string encoder_ = "auto";
    double bitrate_mbps_ = 0.0;
};

} // namespace ttrally::cli
