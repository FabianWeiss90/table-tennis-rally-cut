// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"

#include <filesystem>

namespace ttrally::launcher {

/// Reads a video library:
///   <library>/<match day>/            the original videos (untouched)
///   <library>/<match day>/<...liimba...>/  pre-cut versions (any subfolder named like "Liimba")
///   <library>/<match day>/ttrally-data/    the tool's files: align, annotations, features,
///                                          detections, cuts, cache, refined, the chosen pre-cuts
///   <library>/ttrally-data/models/    image models and trained detectors
/// plus the detectors in the project's weights/ and the uv program in its external/uv/.
class LibraryScanner final : public WorkspaceScanner {
  public:
    explicit LibraryScanner(std::filesystem::path project_directory)
        : project_(std::move(project_directory)) {}

    [[nodiscard]] WorkspaceSnapshot scan(const std::filesystem::path& library) override;

  private:
    std::filesystem::path project_;
};

/// Settings in a small text file (one line: the library folder).
class FileLauncherSettings final : public LauncherSettings {
  public:
    explicit FileLauncherSettings(std::filesystem::path file) : file_(std::move(file)) {}

    [[nodiscard]] std::optional<std::filesystem::path> library() override;
    void save_library(const std::filesystem::path& library) override;

  private:
    std::filesystem::path file_;
};

/// Where the launcher keeps its settings: $XDG_CONFIG_HOME/ttrally (else ~/.config/ttrally) on
/// Linux, %APPDATA%\\ttrally on Windows.
[[nodiscard]] std::filesystem::path default_settings_file();

} // namespace ttrally::launcher
