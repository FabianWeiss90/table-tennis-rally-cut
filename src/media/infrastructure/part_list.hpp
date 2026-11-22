// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <filesystem>
#include <span>
#include <vector>

namespace ttrally::media {

/// Writes a part list: the videos, one after another, read as one video by every command (FFmpeg's
/// concat format, the parts named relative to the list, each with its duration so that its
/// place on the timeline is known without reading the parts before it). Unchanged lists are not
/// rewritten (their time keys the caches). Throws std::runtime_error if a part cannot be read
/// or the list cannot be written.
void write_part_list(const std::filesystem::path& list,
                     std::span<const std::filesystem::path> parts);

/// The parts named in a part list (absolute); empty if the list cannot be read.
[[nodiscard]] std::vector<std::filesystem::path> read_part_list(const std::filesystem::path& list);

/// A part of a part list and where it lies on the list's timeline.
struct PartSpan {
    std::filesystem::path file; ///< absolute
    double start_s = 0.0;
    double end_s = 0.0;
};

/// The parts of a video on its timeline: for a part list each part after the ones before it (by
/// the durations in the list), for any other file the file itself over all time.
[[nodiscard]] std::vector<PartSpan> part_spans(const std::filesystem::path& video);

} // namespace ttrally::media
