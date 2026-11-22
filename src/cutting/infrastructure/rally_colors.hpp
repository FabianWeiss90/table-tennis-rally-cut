// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/color_match.hpp"
#include "shared/application/progress_reporter.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <utility>

namespace ttrally::cutting {

/// The median of `count` frames spread over a part of a video, as a small RGB picture; empty if
/// the video cannot be read there.
[[nodiscard]] RgbImage sampled_picture(const std::filesystem::path& video, double start_s,
                                       double end_s, int count);

/// The colour corrections of rallies towards a match day's reference rally (see
/// rally_correction): each rally measured on its own frames, with the area of its file.
class RallyColors {
  public:
    /// `white_balance`: the match day's white-balance.csv; empty or missing: no corrections.
    explicit RallyColors(const std::filesystem::path& white_balance);

    /// Whether rallies are corrected at all (a reference rally is chosen).
    [[nodiscard]] bool active() const noexcept { return target_.has_value(); }

    /// The correction of the part of `video` from `start_s` to `end_s` (identity if inactive).
    [[nodiscard]] ToneCurves for_span(const std::filesystem::path& video, double start_s,
                                      double end_s);

  private:
    /// The area of the file of `video` that holds time `t` (a part of a part list).
    [[nodiscard]] std::optional<RelativeRect> area_at(const std::filesystem::path& video,
                                                      double t) const;

    WhiteBalanceSettings settings_;
    std::map<std::string, std::optional<RelativeRect>> areas_; ///< by file name
    std::optional<ColorTarget> target_;
};

} // namespace ttrally::cutting
