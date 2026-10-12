// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"

#include <filesystem>

namespace ttrally::launcher {

/// Reads the workspace from the project directory's usual places: videos in data/ (versions
/// whose name extends another video's name are pre-cuts of it), data/align, data/annotations,
/// data/features, data/detections, data/cuts, image models in data/models and detectors in
/// weights/ and data/models.
class FilesystemWorkspaceScanner final : public WorkspaceScanner {
  public:
    explicit FilesystemWorkspaceScanner(std::filesystem::path project_directory)
        : root_(std::move(project_directory)) {}

    [[nodiscard]] WorkspaceSnapshot scan() override;

  private:
    [[nodiscard]] VideoEntry describe(const std::filesystem::path& video) const;
    [[nodiscard]] std::string relative(const std::filesystem::path& path) const;

    std::filesystem::path root_;
};

} // namespace ttrally::launcher
