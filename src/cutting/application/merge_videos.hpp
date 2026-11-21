// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/application/progress_reporter.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace ttrally::cutting {

/// Port: appends videos to each other into a new file without re-encoding.
class VideoJoiner {
  public:
    virtual ~VideoJoiner() = default;
    /// Writes the parts one after another into `output` (which must not exist yet) and returns
    /// its duration in seconds. Throws media::MediaError if a part cannot be read, the parts
    /// differ in a way that needs re-encoding, or the output exists or cannot be written.
    [[nodiscard]] virtual double join(std::span<const std::filesystem::path> parts,
                                      const std::filesystem::path& output,
                                      ProgressReporter& progress) = 0;
};

struct MergeVideosRequest {
    std::vector<std::filesystem::path> parts; ///< in the order they were recorded
    std::filesystem::path output;
};

struct MergeVideosResult {
    std::size_t parts = 0;
    double duration_s = 0.0; ///< of the merged video
    double seconds = 0.0;    ///< run time
};

/// Use case behind `ttrally merge`: a game recorded in several files (e.g. a camera that starts
/// a new file every few gigabytes) as one video, the parts appended without re-encoding. The
/// parts are left as they are; the merged video is always a new file.
class MergeVideos {
  public:
    MergeVideos(VideoJoiner& joiner, ProgressReporter& progress)
        : joiner_(joiner), progress_(progress) {}

    /// Throws std::invalid_argument (fewer than two parts, a part twice, the output among the
    /// parts) or media::MediaError.
    [[nodiscard]] MergeVideosResult execute(const MergeVideosRequest& request);

  private:
    VideoJoiner& joiner_;
    ProgressReporter& progress_;
};

} // namespace ttrally::cutting
