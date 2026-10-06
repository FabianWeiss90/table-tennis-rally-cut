// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/application/align_videos.hpp"

#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>

namespace ttrally::cli {

/// `ttrally align`: command-line options and wiring of the AlignVideos use case.
class AlignCommand {
  public:
    explicit AlignCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    alignment::AlignVideosRequest request_;
    std::filesystem::path out_csv_;
    std::string report_;
    std::string cache_dir_ = "data/cache";
    std::string decode_backend_ = "auto";
    bool no_cache_ = false;
    bool no_visual_check_ = false;
};

} // namespace ttrally::cli
