// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/domain/command_form.hpp"
#include "launcher/domain/forms.hpp"
#include "launcher/domain/workspace.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::launcher {

/// Which videos a batch runs for.
enum class BatchScope {
    All,     ///< every video of the match day, computed again
    Missing, ///< only the videos without the command's result
};

/// The steps a pipeline can run for every game of a match day, in their order.
inline constexpr std::array<std::string_view, 4> kPipelineSteps{"align", "features", "detect",
                                                                 "cut"};

/// The steps a pipeline runs unless the user chooses others: features, detect, cut.
[[nodiscard]] std::vector<std::string> default_pipeline();

/// The ticked `steps` (in the order of kPipelineSteps) a pipeline runs for a video: with All
/// every one; with Missing those from the first one without its result on (a step that runs
/// makes the results of the later ones stale).
[[nodiscard]] std::vector<std::string> pipeline_for(std::span<const std::string> steps,
                                                    const VideoEntry& video, BatchScope scope);

/// Per field of the command's form for `video`: whether its proposed value depends on the video
/// itself (its id or original), not only on its match day. The match day's form leaves those out.
[[nodiscard]] std::vector<bool> video_dependent_fields(std::string_view command,
                                                       const VideoEntry& video,
                                                       const WorkspaceSnapshot& workspace);

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
