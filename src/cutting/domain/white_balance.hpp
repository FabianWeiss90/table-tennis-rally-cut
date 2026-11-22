// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <array>
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

/// Gains that give `measured` the tint `reference` has (the same surface in another video),
/// keeping its own brightness: the white balance of the other video, not its exposure.
[[nodiscard]] RgbGains matching_gains(RgbColor measured, RgbColor reference);

/// The gains as a matrix on (Y, U - 128, V - 128): the correction applied directly to the YUV
/// values of a frame (exact, since the conversion between RGB and YUV is linear). Row by row,
/// each row giving the new Y, U - 128 or V - 128.
[[nodiscard]] std::array<double, 9> yuv_matrix(RgbGains gains, YuvFormat format = {});

/// What a video's white balance is set to.
enum class WhiteBalanceChoice {
    Unchanged, ///< as recorded
    Neutral,   ///< the scene's area made a neutral grey
    Like,      ///< the scene's area given the tint it has in another file
};

/// The white balance chosen for one file (a video or a part of a match), with the gains that
/// realise it.
struct FileWhiteBalance {
    std::string file; ///< file name in the match day folder
    WhiteBalanceChoice choice = WhiteBalanceChoice::Unchanged;
    std::string like; ///< for Like: the other file's name
    RgbGains gains;

    friend bool operator==(const FileWhiteBalance&, const FileWhiteBalance&) = default;
};

/// A rectangle as shares (0-1) of a picture's width and height.
struct RelativeRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    friend bool operator==(const RelativeRect&, const RelativeRect&) = default;
};

/// The area of each scene that should be neutral (e.g. the white table edge or a wall), and the
/// white balance chosen for each file of a match day.
struct WhiteBalanceSettings {
    std::vector<std::pair<std::string, RelativeRect>> areas; ///< scene -> area
    std::vector<FileWhiteBalance> files;

    friend bool operator==(const WhiteBalanceSettings&, const WhiteBalanceSettings&) = default;
};

/// A stretch of a video's timeline with the gains for its frames.
struct ColorSpan {
    double start_s = 0.0;
    double end_s = 0.0;
    RgbGains gains;
};

/// The gains for the frame at `time_s`; identity gains outside every span.
[[nodiscard]] RgbGains gains_at(const std::vector<ColorSpan>& spans, double time_s);

} // namespace ttrally::cutting
