// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>

namespace ttrally::cli {

/// `ttrally features`: per-frame image features of a video for training and detection.
class FeaturesCommand {
  public:
    explicit FeaturesCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::filesystem::path video_;
    std::string video_id_;
    std::filesystem::path model_ = "data/models/dinov2-vitb14.onnx";
    std::filesystem::path out_dir_ = "data/features";
    std::filesystem::path cache_dir_ = "data/cache";
    std::string execution_provider_ = "auto";
    std::string decode_backend_ = "auto";
    double sample_rate_hz_ = 10.0;
    std::size_t batch_size_ = 16;
    bool force_ = false;
};

} // namespace ttrally::cli
