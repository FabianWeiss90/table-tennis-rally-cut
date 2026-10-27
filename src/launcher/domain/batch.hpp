// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/domain/command_form.hpp"
#include "launcher/domain/workspace.hpp"

#include <string>
#include <string_view>

namespace ttrally::launcher {

/// Whether a command can run for all videos of a match day at once (align, features, detect,
/// cut; not annotate, which needs the user, nor train, which takes all videos anyway).
[[nodiscard]] bool supports_batch(std::string_view command);

/// Whether the command's result exists for the video.
[[nodiscard]] bool step_done(std::string_view command, const VideoEntry& video);

/// Why the command cannot run for the video (e.g. align without a pre-cut video); empty if it
/// can.
[[nodiscard]] std::string missing_input(std::string_view command, const VideoEntry& video);

/// The form `other` (filled in for another video) with the edits made in `edited` (the form of
/// the selected video) to fields that do not depend on the video, i.e. whose unedited value
/// `selected` is the same for both videos. Paths and ids of the other video are kept.
[[nodiscard]] CommandForm carry_over_edits(const CommandForm& edited, const CommandForm& selected,
                                           CommandForm other);

} // namespace ttrally::launcher
