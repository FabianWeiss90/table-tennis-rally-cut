// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/domain/workspace.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ttrally::launcher {

/// Backgrounds at least this similar (correlation of the grey values) show the same scene: the
/// same hall seen from the same camera position.
inline constexpr double kSameSceneSimilarity = 0.8;

/// A small grey image of what stays the same in a video (e.g. the median over many frames, which
/// removes the moving players).
struct Background {
    int width = 0;
    int height = 0;
    std::vector<float> pixels; ///< row by row
};

/// Correlation of two backgrounds (1 = same picture up to brightness and contrast); 0 if their
/// sizes differ or one is empty or uniform.
[[nodiscard]] double similarity(const Background& a, const Background& b);

/// Groups items into scenes from their pairwise similarities: items whose groups are on average
/// at least `threshold` similar are merged (average linkage). Returns the scene names "A", "B",
/// ... in the order the items first appear.
[[nodiscard]] std::vector<std::string>
group_into_scenes(const std::vector<std::vector<double>>& similarities,
                  double threshold = kSameSceneSimilarity);

/// "A" ... "Z", then "AA", "AB", ...
[[nodiscard]] std::string scene_name(std::size_t index);

/// Scenes that have training videos (`ticked` among `trainable`) and validation videos (the
/// other `trainable` ones), as "<match day>, scene <name>"; such a validation flatters the model.
[[nodiscard]] std::vector<std::string> scenes_in_both(const std::vector<VideoEntry>& videos,
                                                      const std::vector<std::string>& trainable,
                                                      const std::vector<std::string>& ticked);

} // namespace ttrally::launcher
