// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <array>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace ttrally::cutting {

/// A colour as red, green and blue from 0 to 255 (gamma encoded, as in the video).
struct RgbColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

/// Factors for red, green and blue that correct a video's white balance.
struct RgbGains {
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;

    /// No change (within a thousandth).
    [[nodiscard]] bool identity() const noexcept;
    friend bool operator==(const RgbGains&, const RgbGains&) = default;
};

/// How YUV values of a video relate to RGB.
struct YuvFormat {
    bool bt709 = true;       ///< else BT.601 (standard definition)
    bool full_range = false; ///< 0-255 instead of 16-235 (16-240 for chroma)
};

/// The colour of a YUV value (luma, then the two chroma values, each 0-255).
[[nodiscard]] RgbColor rgb_of(double y, double u, double v, YuvFormat format = {});

/// Luma (brightness) of a colour, by BT.709 weights.
[[nodiscard]] double luma_of(RgbColor color);

/// Gains that turn `measured` (e.g. the colour of a white surface) into a neutral grey of the
/// same brightness.
[[nodiscard]] RgbGains neutral_gains(RgbColor measured);

/// Gains that give `measured` the colour `target` has: its tint only, keeping the own
/// brightness, or with `match_brightness` its brightness too.
[[nodiscard]] RgbGains gains_towards(RgbColor measured, RgbColor target, bool match_brightness);

/// The gains as a matrix on (Y, U - 128, V - 128): the correction applied directly to the YUV
/// values of a frame (exact, since the conversion between RGB and YUV is linear). Row by row,
/// each row giving the new Y, U - 128 or V - 128.
[[nodiscard]] std::array<double, 9> yuv_matrix(RgbGains gains, YuvFormat format = {});

/// The colour after the gains.
[[nodiscard]] RgbColor corrected(RgbColor color, RgbGains gains);

/// Range of the temperature and tint sliders: -1 to 1 (shown as -100 to 100).
inline constexpr double kMaxAdjustment = 1.0;

/// Gains of a temperature (positive: warmer, more red and less blue) and a tint (positive:
/// more magenta, less green), each -1 to 1 (at most half as much or 1.5 times a channel),
/// scaled so that `measured` keeps its brightness, then brightened by `brightness` (-1 to 1: half
/// as bright to 1.5 times as bright).
[[nodiscard]] RgbGains temperature_tint_gains(double temperature, double tint, double brightness,
                                              RgbColor measured);

/// The temperature and tint that make `measured` a neutral grey (within the sliders' range).
[[nodiscard]] std::pair<double, double> neutral_temperature_tint(RgbColor measured);

/// A rectangle as shares (0-1) of a picture's width and height.
struct RelativeRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    friend bool operator==(const RelativeRect&, const RelativeRect&) = default;
};

/// The area of one file (a video or a part of a match) over a spot that looks the same in every
/// file of the match day.
struct FileWhiteBalance {
    std::string file; ///< file name in the match day folder
    std::optional<RelativeRect> area; ///< none: where it is in the file before

    friend bool operator==(const FileWhiteBalance&, const FileWhiteBalance&) = default;
};

/// The white balance of a match day: every file's area, and the reference rally, tuned with
/// temperature, tint and brightness, towards which every rally is corrected when cutting.
struct WhiteBalanceSettings {
    std::vector<FileWhiteBalance> files; ///< in the order of the match day's list
    /// The video of the reference rally: its path in the match day folder (a file, or a match's
    /// part list under ttrally-data); empty: none chosen
    std::string reference_video;
    double reference_start_s = 0.0;
    double reference_end_s = 0.0;
    double temperature = 0.0;
    double tint = 0.0;
    double brightness = 0.0;
    /// Every rally's whole picture made as bright on average as the reference's; else only the
    /// area's colour tone is matched
    bool match_brightness = false;

    [[nodiscard]] bool has_reference() const noexcept {
        return !reference_video.empty() && reference_end_s > reference_start_s;
    }
    friend bool operator==(const WhiteBalanceSettings&, const WhiteBalanceSettings&) = default;
};

/// The area of each file (same order). The first file's area sets the size for all; every other
/// file has it where its own area lies (only moved, so only its position counts), else where it
/// lies in the file before. None without an area in the first file.
[[nodiscard]] std::vector<std::optional<RelativeRect>>
effective_areas(const std::vector<FileWhiteBalance>& files);

} // namespace ttrally::cutting
