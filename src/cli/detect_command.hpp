// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <filesystem>
#include <string>

namespace ttrally::cli {

/// `ttrally detect`: finds the rallies of a video with a trained detector.
class DetectCommand {
  public:
    explicit DetectCommand(CLI::App& app);

    [[nodiscard]] bool selected() const { return command_->parsed(); }
    int run();

  private:
    CLI::App* command_;
    std::filesystem::path video_;
    std::string video_id_;
    std::filesystem::path detector_ = "weights/rally-detector.onnx";
    std::filesystem::path backbone_ = "data/models/dinov2-vitb14-fp16.onnx";
    std::filesystem::path features_dir_ = "data/features";
    std::filesystem::path cache_dir_ = "data/cache";
    std::filesystem::path out_dir_ = "data/detections";
    std::filesystem::path labels_dir_ = "data/annotations";
    std::string execution_provider_ = "auto";
    std::string decode_backend_ = "auto";
    std::size_t batch_size_ = 16;
    bool no_evaluation_ = false;
};

} // namespace ttrally::cli
