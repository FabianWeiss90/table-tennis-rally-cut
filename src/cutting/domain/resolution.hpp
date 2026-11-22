// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <filesystem>

namespace ttrally::cutting {

/// Height of a written video: the source's, or `max_height` if that is lower (0: no limit);
/// even, as 4:2:0 needs. A video is never enlarged.
[[nodiscard]] int output_height(int source_height, int max_height) noexcept;

/// The file name with the height at the end of its stem: "cuts/game.mp4" with 1080 is
/// "cuts/game-1080p.mp4".
[[nodiscard]] std::filesystem::path with_resolution(const std::filesystem::path& file,
                                                    int height);

} // namespace ttrally::cutting
