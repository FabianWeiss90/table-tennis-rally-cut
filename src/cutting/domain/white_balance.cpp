// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/domain/white_balance.hpp"

#include <algorithm>
#include <cmath>

namespace ttrally::cutting {

namespace {

constexpr double kTolerance = 1e-3;
constexpr double kLimitedBlack = 16.0;
constexpr double kLimitedLumaScale = 255.0 / 219.0;
constexpr double kLimitedChromaScale = 255.0 / 224.0;
constexpr double kChromaZero = 128.0;
constexpr double kMinChannel = 1.0; ///< a black channel cannot be scaled

struct LumaWeights {
    double red;
    double blue;
    [[nodiscard]] double green() const { return 1.0 - red - blue; }
};

LumaWeights weights(bool bt709) {
    return bt709 ? LumaWeights{0.2126, 0.0722} : LumaWeights{0.299, 0.114};
}

using Matrix = std::array<double, 9>;

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            for (int k = 0; k < 3; ++k) {
                result[row * 3 + column] += a[row * 3 + k] * b[k * 3 + column];
            }
        }
    }
    return result;
}

/// RGB (0-255) from (Y', U', V') = luma and chroma scaled to 0-255 and centred: chroma -128..127.
Matrix rgb_from_yuv(bool bt709) {
    const LumaWeights w = weights(bt709);
    const double kr = w.red;
    const double kb = w.blue;
    const double kg = w.green();
    return {1.0, 0.0,
            2.0 * (1.0 - kr), // red
            1.0, -2.0 * kb * (1.0 - kb) / kg, -2.0 * kr * (1.0 - kr) / kg, // green
            1.0, 2.0 * (1.0 - kb), 0.0};                                    // blue
}

/// The inverse: (Y', U', V') from RGB.
Matrix yuv_from_rgb(bool bt709) {
    const LumaWeights w = weights(bt709);
    const double kr = w.red;
    const double kb = w.blue;
    const double kg = w.green();
    return {kr, kg, kb,                                                              // Y
            -kr / (2.0 * (1.0 - kb)), -kg / (2.0 * (1.0 - kb)), 0.5,                 // U
            0.5, -kg / (2.0 * (1.0 - kr)), -kb / (2.0 * (1.0 - kr))};                // V
}

/// From the stored values (luma offset by black, chroma centred) to full-scale values.
Matrix range_expansion(bool full_range) {
    const double luma = full_range ? 1.0 : kLimitedLumaScale;
    const double chroma = full_range ? 1.0 : kLimitedChromaScale;
    return {luma, 0, 0, 0, chroma, 0, 0, 0, chroma};
}

Matrix range_compression(bool full_range) {
    const double luma = full_range ? 1.0 : 1.0 / kLimitedLumaScale;
    const double chroma = full_range ? 1.0 : 1.0 / kLimitedChromaScale;
    return {luma, 0, 0, 0, chroma, 0, 0, 0, chroma};
}

double channel(double value) { return std::max(kMinChannel, value); }

/// Gains scaled so that the corrected colour keeps the luma of `measured`.
RgbGains keeping_luma(RgbGains gains, RgbColor measured) {
    const RgbColor corrected{measured.red * gains.red, measured.green * gains.green,
                             measured.blue * gains.blue};
    const double scale = luma_of(measured) / std::max(kMinChannel, luma_of(corrected));
    return {gains.red * scale, gains.green * scale, gains.blue * scale};
}

} // namespace

bool RgbGains::identity() const noexcept {
    return std::abs(red - 1.0) < kTolerance && std::abs(green - 1.0) < kTolerance &&
           std::abs(blue - 1.0) < kTolerance;
}

RgbColor rgb_of(double y, double u, double v, YuvFormat format) {
    const double black = format.full_range ? 0.0 : kLimitedBlack;
    const Matrix to_rgb = multiply(rgb_from_yuv(format.bt709), range_expansion(format.full_range));
    const std::array<double, 3> yuv{y - black, u - kChromaZero, v - kChromaZero};
    std::array<double, 3> rgb{};
    for (int row = 0; row < 3; ++row) {
        for (int k = 0; k < 3; ++k) {
            rgb[row] += to_rgb[row * 3 + k] * yuv[k];
        }
    }
    return {rgb[0], rgb[1], rgb[2]};
}

double luma_of(RgbColor color) {
    const LumaWeights w = weights(true);
    return w.red * color.red + w.green() * color.green + w.blue * color.blue;
}

RgbGains neutral_gains(RgbColor measured) {
    const double grey = luma_of(measured);
    return keeping_luma({grey / channel(measured.red), grey / channel(measured.green),
                         grey / channel(measured.blue)},
                        measured);
}

RgbGains matching_gains(RgbColor measured, RgbColor reference) {
    return keeping_luma({channel(reference.red) / channel(measured.red),
                         channel(reference.green) / channel(measured.green),
                         channel(reference.blue) / channel(measured.blue)},
                        measured);
}

std::array<double, 9> yuv_matrix(RgbGains gains, YuvFormat format) {
    const Matrix scale{gains.red, 0, 0, 0, gains.green, 0, 0, 0, gains.blue};
    // stored -> full scale -> RGB -> gains -> back
    Matrix result = multiply(rgb_from_yuv(format.bt709), range_expansion(format.full_range));
    result = multiply(scale, result);
    result = multiply(yuv_from_rgb(format.bt709), result);
    return multiply(range_compression(format.full_range), result);
}

RgbGains gains_at(const std::vector<ColorSpan>& spans, double time_s) {
    for (const ColorSpan& span : spans) {
        if (time_s >= span.start_s && time_s < span.end_s) {
            return span.gains;
        }
    }
    return {};
}

} // namespace ttrally::cutting
