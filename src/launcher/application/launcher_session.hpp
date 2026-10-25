// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"
#include "launcher/domain/forms.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::launcher {

/// Application service behind the launcher window: the workspace and its videos, one set of
/// forms for the selected video, and the command currently running with its output.
class LauncherSession {
  public:
    /// `ttrally` is the program run for the tool's own commands (normally this executable); the
    /// commands run in the project directory (which holds weights/ and the training).
    LauncherSession(WorkspaceScanner& scanner, ProcessRunner& runner, std::string ttrally,
                    std::filesystem::path project_directory, std::filesystem::path library);

    [[nodiscard]] const WorkspaceSnapshot& workspace() const noexcept { return workspace_; }
    [[nodiscard]] const std::filesystem::path& library() const noexcept { return library_; }
    /// Switches to another library (not while a command runs) and scans it.
    void set_library(std::filesystem::path library);
    [[nodiscard]] std::optional<std::size_t> selected_video() const noexcept { return selected_; }
    /// Fills the forms in for the video (edits made for the previous video are dropped, except
    /// in the training form, which does not depend on the video).
    void select_video(std::size_t index);
    [[nodiscard]] std::vector<CommandForm>& forms() noexcept { return forms_; }

    /// Scans the workspace again; keeps the selection and the form values, updates the
    /// suggestions.
    void rescan();

    /// The program and arguments a form runs.
    [[nodiscard]] Invocation invocation(const CommandForm& form) const;
    /// Starts a form's command. Throws std::logic_error if a command is running or required
    /// fields are empty, std::runtime_error if it cannot be started.
    void run(const CommandForm& form);
    void cancel();
    /// Collects new output; when the command has ended, notes the result and rescans.
    void poll();

    [[nodiscard]] bool running() const noexcept { return running_; }
    /// Output of the current or last command.
    [[nodiscard]] const std::string& log() const noexcept { return log_; }
    /// e.g. "cut finished", "detect failed (exit code 1)"; empty before the first command.
    [[nodiscard]] const std::string& status() const noexcept { return status_; }

  private:
    void rebuild_forms();
    void append_log(const std::string& text);

    WorkspaceScanner& scanner_;
    ProcessRunner& runner_;
    std::string ttrally_;
    std::filesystem::path project_directory_;
    std::filesystem::path library_;
    WorkspaceSnapshot workspace_;
    std::optional<std::size_t> selected_;
    std::vector<CommandForm> forms_;
    bool running_ = false;
    std::string running_name_;
    std::string log_;
    std::string status_;
};

} // namespace ttrally::launcher
