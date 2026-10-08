// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>

namespace ttrally::cli {

/// `ttrally annotate`: opens the annotation GUI for an original video and the candidates found
/// by `ttrally align`.
class AnnotateCommand {
  public:
    explicit AnnotateCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::filesystem::path original_;
    std::filesystem::path segments_csv_;
    std::filesystem::path gaps_csv_;
    std::string video_id_;
    std::filesystem::path annotations_dir_ = "data/annotations";
    /// Where earlier versions kept the review progress; read once, then moved to the labels
    std::filesystem::path legacy_state_dir_ = "data/annotate";
    std::filesystem::path cache_dir_ = "data/cache";
    std::string decode_backend_ = "auto";
    int display_height_ = 1080;
    int frame_memory_mb_ = 1024;
};

} // namespace ttrally::cli
