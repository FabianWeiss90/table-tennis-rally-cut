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

/// The merge command: the parts (originals, in the order they were recorded) appended into a
/// new video `output`.
[[nodiscard]] CommandForm merge_form(const std::vector<std::string>& parts,
                                     const std::string& output);

/// The proposed merged video: next to the first part, named after it with "-merged" (e.g.
/// "/day/GX010001.MP4" -> "/day/GX010001-merged.MP4").
[[nodiscard]] std::string merged_name(const std::string& first_part);

} // namespace ttrally::launcher
