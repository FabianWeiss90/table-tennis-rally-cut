// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/launcher_session.hpp"
#include "launcher/application/video_preview.hpp"

#include <filesystem>
#include <string>

namespace ttrally::gui {

struct LauncherOptions {
    std::string title = "ttrally";
    std::filesystem::path project_directory = "."; ///< chosen files are shown relative to it
    int max_frames = 0; ///< Stop after this many rendered frames (0 = until closed; for tests)
    std::filesystem::path screenshot; ///< If set, the last rendered frame is saved here (BMP)
};

/// Opens the launcher window: the videos of the workspace with their progress and a preview
/// frame, and a form per command, filled in for the selected video, to run it with live output.
/// Runs until closed; a running command is cancelled then.
void run_launcher(launcher::LauncherSession& session, launcher::VideoPreview& preview,
                  const LauncherOptions& options);

} // namespace ttrally::gui
