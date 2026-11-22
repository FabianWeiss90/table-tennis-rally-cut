// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "cutting/domain/white_balance.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ttrally::cutting {

/// A small picture in RGB (8 bits per channel, row by row), for measuring colours.
struct RgbImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb; ///< red, green, blue per pixel

    [[nodiscard]] bool empty() const noexcept { return rgb.empty(); }
};

/// The per-pixel median of pictures of the same size (moving players mostly disappear); empty
/// if there are none. Pictures of another size than the first are left out.
[[nodiscard]] RgbImage median_of(std::span<const RgbImage> pictures);

/// The picture with the gains applied (clipped at 255).
[[nodiscard]] RgbImage with_gains(const RgbImage& picture, RgbGains gains);

/// The mean colour of an area of the picture (whole picture: {0, 0, 1, 1}).
[[nodiscard]] RgbColor mean_in(const RgbImage& picture, RelativeRect area);

/// A curve per channel: the new value of every value 0-255 of red, green and blue (a correction
/// by gains is a straight curve per channel).
struct ToneCurves {
    std::array<std::array<std::uint8_t, 256>, 3> curve{};

    /// Curves that change nothing.
    [[nodiscard]] static ToneCurves identity();
    [[nodiscard]] bool is_identity() const noexcept;
    /// A colour through the curves (between their steps linearly).
    [[nodiscard]] RgbColor applied(RgbColor color) const;
    friend bool operator==(const ToneCurves&, const ToneCurves&) = default;
};

/// The curves with every channel scaled by its gain afterwards (clipped at 255).
[[nodiscard]] ToneCurves scaled(const ToneCurves& curves, RgbGains gains);

/// What a rally is corrected towards: the reference rally's picture tuned with temperature,
/// tint and brightness, and its area.
struct ColorTarget {
    RgbImage picture;
    std::optional<RelativeRect> area;
};

/// The reference rally's picture (the median of its frames) tuned with the settings'
/// temperature, tint and brightness (measured on its area, else the whole picture).
[[nodiscard]] ColorTarget tuned_target(const RgbImage& reference, std::optional<RelativeRect> area,
                                       const WhiteBalanceSettings& settings);

/// The correction of a rally (the median of its frames, with its file's area) towards the
/// target, as gains per channel: the area's colour tone made the target area's, and with
/// settings.match_brightness the whole picture as bright on average as the target's. Identity if
/// nothing can be measured.
[[nodiscard]] ToneCurves rally_correction(const RgbImage& rally, std::optional<RelativeRect> area,
                                          const ColorTarget& target,
                                          const WhiteBalanceSettings& settings);

} // namespace ttrally::cutting
