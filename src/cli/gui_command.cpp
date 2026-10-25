// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/gui_command.hpp"

#include "gui/launcher_app.hpp"
#include "launcher/application/launcher_session.hpp"
#include "launcher/infrastructure/ffmpeg_video_preview.hpp"
#include "launcher/infrastructure/library_scanner.hpp"
#include "launcher/infrastructure/subprocess_runner.hpp"

namespace ttrally::cli {

GuiCommand::GuiCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "gui", "Open a window to run the commands with forms filled in for a video")) {
    command_->add_option("--library", library_,
                         "Folder with one folder per match day (default: the one chosen last)")
        ->check(CLI::ExistingDirectory);
    command_->add_option("--project", project_,
                         "Project directory with weights/ and training/ (default: the current "
                         "one)")
        ->check(CLI::ExistingDirectory)
        ->capture_default_str();
}

int GuiCommand::run() {
    launcher::FileLauncherSettings settings(launcher::default_settings_file());
    if (!library_.empty()) {
        settings.save_library(library_);
    }
    const std::filesystem::path library =
        library_.empty() ? settings.library().value_or(std::filesystem::path()) : library_;
    launcher::LibraryScanner scanner(project_);
    launcher::SubprocessRunner runner;
    launcher::LauncherSession session(scanner, runner, launcher::current_executable().string(),
                                      std::filesystem::absolute(project_), library);
    gui::LauncherOptions options;
    options.project_directory = project_;
    options.settings = &settings;
    launcher::FfmpegVideoPreview preview;
    gui::run_launcher(session, preview, options);
    return 0;
}

} // namespace ttrally::cli
