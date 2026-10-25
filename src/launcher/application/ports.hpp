// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/domain/workspace.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::launcher {

/// Port: what is in a video library (match day folders with videos and the tool's files).
class WorkspaceScanner {
  public:
    virtual ~WorkspaceScanner() = default;
    [[nodiscard]] virtual WorkspaceSnapshot scan(const std::filesystem::path& library) = 0;
};

/// Port: settings kept between sessions of the launcher.
class LauncherSettings {
  public:
    virtual ~LauncherSettings() = default;
    /// The library chosen last, if any.
    [[nodiscard]] virtual std::optional<std::filesystem::path> library() = 0;
    virtual void save_library(const std::filesystem::path& library) = 0;
};

/// A program to start.
struct Invocation {
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
};

/// Port: runs one program at a time in its own process and collects its output (stdout and
/// stderr together).
class ProcessRunner {
  public:
    virtual ~ProcessRunner() = default;
    /// Throws std::runtime_error if the program cannot be started.
    virtual void start(const Invocation& invocation) = 0;
    [[nodiscard]] virtual bool running() const = 0;
    /// Output written since the last call.
    [[nodiscard]] virtual std::string take_output() = 0;
    /// Exit code once the program has ended (negative: ended by a signal), nullopt before.
    [[nodiscard]] virtual std::optional<int> exit_code() const = 0;
    /// Asks the program to stop (it may take a moment).
    virtual void cancel() = 0;
};

} // namespace ttrally::launcher
