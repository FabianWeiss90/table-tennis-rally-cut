// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/game_highlights.hpp"
#include "highlights/application/playback_source.hpp"
#include "highlights/application/title_fonts.hpp"
#include "launcher/application/detect_scenes.hpp"
#include "launcher/application/launcher_session.hpp"
#include "launcher/application/video_preview.hpp"

#include <filesystem>
#include <string>

namespace ttrally::gui {

struct LauncherOptions {
    std::string title = "ttrally";
    launcher::LauncherSettings* settings = nullptr; ///< remembers a library chosen in the window
    launcher::SceneStore* scenes = nullptr; ///< scenes of the videos; nullptr: not shown
    /// Highlight marks and playback for the Highlights tab; nullptr: the tab only explains
    highlights::MatchDayHighlights* highlights = nullptr;
    highlights::PlaybackSource* playback = nullptr;
    highlights::TitleFonts* fonts = nullptr; ///< for the titles' preview and the font choice
    std::filesystem::path project_directory = "."; ///< chosen files are shown relative to it
    int max_frames = 0; ///< Stop after this many rendered frames (0 = until closed; for tests)
    std::string first_tab; ///< form shown first, e.g. "highlights" (empty: the first; for tests)
    std::filesystem::path screenshot; ///< If set, the last rendered frame is saved here (BMP)
};

/// Opens the launcher window: the videos of the library by match day with their progress and a
/// preview frame, and a form per command, filled in for the selected video, to run it with live output.
/// Runs until closed; a running command is cancelled then.
void run_launcher(launcher::LauncherSession& session, launcher::VideoPreview& preview,
                  const LauncherOptions& options);

} // namespace ttrally::gui
