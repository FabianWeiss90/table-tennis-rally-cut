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
    /// The CPU, not auto: annotating needs every frame in main memory, and copying them back from
    /// the GPU made VAAPI decoding about 3 times slower (94 vs. 276 frames/s, RX 7800 XT, 24
    /// threads); with the CPU, stepping back from a segment start never waited.
    std::string decode_backend_ = "cpu";
    int display_height_ = 1080;
    /// About 16 s of decoded Full HD frames (3.1 MB each): a gap of up to 10 s as a whole plus
    /// the buffers before and after it.
    int frame_memory_mb_ = 3072;
};

} // namespace ttrally::cli
