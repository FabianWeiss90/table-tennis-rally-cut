// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <filesystem>
#include <string>

namespace ttrally::cutting {

/// The rallies a video is cut to, in the label format.
struct CutListChoice {
    std::filesystem::path file; ///< may not exist (nothing detected yet)
    std::string kind;           ///< "complete annotations", "refined detections" or "detections"
};

/// The annotations <annotations>/<id>.csv if they are complete (every segment and gap of the
/// review done), else the refined detections <refined>/<id>.csv if they exist, else the
/// detections <detections>/<id>.csv.
[[nodiscard]] CutListChoice choose_cut_list(const std::filesystem::path& annotations_dir,
                                            const std::filesystem::path& refined_dir,
                                            const std::filesystem::path& detections_dir,
                                            const std::string& video_id);

} // namespace ttrally::cutting
