// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/video_preview.hpp"
#include "launcher/domain/scenes.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ttrally::launcher {

/// Port: the scene of each video of a match day, kept in its work folder.
class SceneStore {
  public:
    virtual ~SceneStore() = default;
    /// Video id -> scene; empty if none are stored.
    [[nodiscard]] virtual std::map<std::string, std::string>
    load(const std::filesystem::path& work_dir) = 0;
    virtual void save(const std::filesystem::path& work_dir,
                      const std::map<std::string, std::string>& scenes) = 0;
};

/// The background of a video: the median of frames spread over it (removes the players), as a
/// small grey image; empty if the video cannot be read.
[[nodiscard]] Background video_background(VideoPreview& preview,
                                          const std::filesystem::path& video);

/// Suggests the scene of each video (same order): videos with similar backgrounds share one.
/// The videos are read in parallel; this takes a few seconds per video.
[[nodiscard]] std::vector<std::string>
detect_scenes(VideoPreview& preview, const std::vector<std::filesystem::path>& videos);

} // namespace ttrally::launcher
