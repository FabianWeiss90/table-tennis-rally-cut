// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/application/merge_videos.hpp"

namespace ttrally::cutting {

/// Appends videos with the FFmpeg libraries by copying their compressed packets (no
/// re-encoding): the video and the main sound of each part, the timestamps of each part moved to
/// follow the previous one. Other streams (camera metadata, timecode) are left out. The parts
/// must share codec, picture size and codec settings, as the files of one recording do. The
/// output is written next to its destination first and renamed once complete; its container
/// follows its extension (e.g. .mp4, .mov, .mkv).
class FfmpegVideoJoiner final : public VideoJoiner {
  public:
    [[nodiscard]] double join(std::span<const std::filesystem::path> parts,
                              const std::filesystem::path& output,
                              ProgressReporter& progress) override;
};

} // namespace ttrally::cutting
