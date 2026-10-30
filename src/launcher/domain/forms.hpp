// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/domain/command_form.hpp"
#include "launcher/domain/workspace.hpp"

#include <vector>

namespace ttrally::launcher {

/// The forms of all commands (align, annotate, features, detect, refine, cut, train), filled in for a
/// video: its id, paths that follow from it, and suggestions from the workspace. Options that
/// are rarely needed are marked advanced and keep the commands' defaults.
[[nodiscard]] std::vector<CommandForm> forms_for(const VideoEntry& video,
                                                 const WorkspaceSnapshot& workspace);

} // namespace ttrally::launcher
