// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/domain/command_form.hpp"
#include "launcher/domain/workspace.hpp"

#include <vector>

namespace ttrally::launcher {

/// The forms of all commands (align, annotate, features, detect, refine, cut, highlights, train),
/// filled in for a video: its id, paths that follow from it, and suggestions from the workspace.
/// Options that are rarely needed are marked advanced and keep the commands' defaults.
[[nodiscard]] std::vector<CommandForm> forms_for(const VideoEntry& video,
                                                 const WorkspaceSnapshot& workspace);

/// After the codec or resolution of a form (cut, highlights) changed: proposes its bit rate
/// (empty for the original resolution: the encoder chooses), unless one of the user's own was
/// entered.
void propose_bitrate(CommandForm& form);

} // namespace ttrally::launcher
