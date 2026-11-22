// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/domain/color_match.hpp"

#include <algorithm>
#include <cmath>

namespace ttrally::cutting {

namespace {

constexpr int kLevels = 256;
constexpr int kChannels = 3;
constexpr double kMaxValue = 255.0;
constexpr RelativeRect kWholePicture{0.0, 0.0, 1.0, 1.0};

std::uint8_t clamped(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, kMaxValue)));
}

/// The share of pixels at or below every value, per channel.
std::array<std::array<double, kLevels>, kChannels> cumulative(const RgbImage& picture) {
    std::array<std::array<double, kLevels>, kChannels> counts{};
    const std::size_t pixels = picture.rgb.size() / kChannels;
    for (std::size_t p = 0; p < pixels; ++p) {
        for (int c = 0; c < kChannels; ++c) {
            counts[c][picture.rgb[p * kChannels + static_cast<std::size_t>(c)]] += 1.0;
        }
    }
    for (auto& channel : counts) {
        double sum = 0.0;
        for (double& count : channel) {
            sum += count;
            count = pixels > 0 ? sum / static_cast<double>(pixels) : 0.0;
        }
    }
    return counts;
}

} // namespace

RgbImage median_of(std::span<const RgbImage> pictures) {
    if (pictures.empty()) {
        return {};
    }
    const RgbImage& first = pictures.front();
    std::vector<const RgbImage*> same;
    for (const RgbImage& picture : pictures) {
        if (picture.width == first.width && picture.height == first.height &&
            picture.rgb.size() == first.rgb.size()) {
            same.push_back(&picture);
        }
    }
    RgbImage median{first.width, first.height, std::vector<std::uint8_t>(first.rgb.size())};
    std::vector<std::uint8_t> values(same.size());
    for (std::size_t i = 0; i < median.rgb.size(); ++i) {
        for (std::size_t p = 0; p < same.size(); ++p) {
            values[p] = same[p]->rgb[i];
        }
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
        std::ranges::nth_element(values, middle);
        median.rgb[i] = *middle;
    }
    return median;
}

RgbImage with_gains(const RgbImage& picture, RgbGains gains) {
    RgbImage result = picture;
    const std::array<double, kChannels> factors{gains.red, gains.green, gains.blue};
    for (std::size_t i = 0; i < result.rgb.size(); ++i) {
        result.rgb[i] = clamped(result.rgb[i] * factors[i % kChannels]);
    }
    return result;
}

RgbColor mean_in(const RgbImage& picture, RelativeRect area) {
    if (picture.empty() || area.empty()) {
        return {};
    }
    const int left = std::clamp(static_cast<int>(area.left * picture.width), 0, picture.width - 1);
    const int top = std::clamp(static_cast<int>(area.top * picture.height), 0, picture.height - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(area.right * picture.width)),
                                 left + 1, picture.width);
    const int bottom = std::clamp(static_cast<int>(std::ceil(area.bottom * picture.height)),
                                  top + 1, picture.height);
    std::array<double, kChannels> sums{};
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.width) +
                             static_cast<std::size_t>(x)) *
                            kChannels;
            for (int c = 0; c < kChannels; ++c) {
                sums[c] += picture.rgb[at + static_cast<std::size_t>(c)];
            }
        }
    }
    const double count = static_cast<double>(right - left) * (bottom - top);
    return {sums[0] / count, sums[1] / count, sums[2] / count};
}

ToneCurves ToneCurves::identity() {
    ToneCurves curves;
    for (auto& channel : curves.curve) {
        for (int v = 0; v < kLevels; ++v) {
            channel[static_cast<std::size_t>(v)] = static_cast<std::uint8_t>(v);
        }
    }
    return curves;
}

bool ToneCurves::is_identity() const noexcept { return *this == identity(); }

RgbColor ToneCurves::applied(RgbColor color) const {
    const auto through = [&](int c, double value) {
        const double clamped_value = std::clamp(value, 0.0, kMaxValue);
        const auto low = static_cast<std::size_t>(std::floor(clamped_value));
        const std::size_t high = std::min<std::size_t>(low + 1, kLevels - 1);
        const double share = clamped_value - static_cast<double>(low);
        return curve[static_cast<std::size_t>(c)][low] * (1.0 - share) +
               curve[static_cast<std::size_t>(c)][high] * share;
    };
    return {through(0, color.red), through(1, color.green), through(2, color.blue)};
}

ToneCurves matching_curves(const RgbImage& from, const RgbImage& to) {
    if (from.empty() || to.empty()) {
        return ToneCurves::identity();
    }
    const auto source = cumulative(from);
    const auto target = cumulative(to);
    ToneCurves curves;
    for (int c = 0; c < kChannels; ++c) {
        // The target value for every value the source has (the lowest whose share reaches it)
        std::vector<std::pair<int, double>> known;
        int matched = 0;
        double before = 0.0;
        for (int v = 0; v < kLevels; ++v) {
            const double share = source[c][v];
            if (share <= before) {
                continue; // a value the source does not have
            }
            while (matched < kLevels - 1 && target[c][matched] < share) {
                ++matched;
            }
            known.emplace_back(v, matched);
            before = share;
        }
        // Between them linearly; below the first and above the last shifted alike
        auto& curve = curves.curve[static_cast<std::size_t>(c)];
        std::size_t next = 0;
        for (int v = 0; v < kLevels; ++v) {
            while (next < known.size() && known[next].first < v) {
                ++next;
            }
            double value = 0.0;
            if (next == 0) {
                value = v + known.front().second - known.front().first;
            } else if (next == known.size()) {
                value = v + known.back().second - known.back().first;
            } else {
                const auto& [low, low_value] = known[next - 1];
                const auto& [high, high_value] = known[next];
                const double share = static_cast<double>(v - low) / (high - low);
                value = low_value + (high_value - low_value) * share;
            }
            curve[static_cast<std::size_t>(v)] = clamped(value);
        }
    }
    return curves;
}

ToneCurves scaled(const ToneCurves& curves, RgbGains gains) {
    ToneCurves result = curves;
    const std::array<double, kChannels> factors{gains.red, gains.green, gains.blue};
    for (int c = 0; c < kChannels; ++c) {
        for (auto& value : result.curve[static_cast<std::size_t>(c)]) {
            value = clamped(value * factors[static_cast<std::size_t>(c)]);
        }
    }
    return result;
}

ColorTarget tuned_target(const RgbImage& reference, std::optional<RelativeRect> area,
                         const WhiteBalanceSettings& settings) {
    const RgbColor measured = mean_in(reference, area.value_or(kWholePicture));
    const RgbGains gains = temperature_tint_gains(settings.temperature, settings.tint,
                                                  settings.brightness, measured);
    return {with_gains(reference, gains), area};
}

ToneCurves rally_correction(const RgbImage& rally, std::optional<RelativeRect> area,
                            const ColorTarget& target, const WhiteBalanceSettings& settings) {
    if (rally.empty() || target.picture.empty()) {
        return ToneCurves::identity();
    }
    ToneCurves curves = settings.match_brightness ? matching_curves(rally, target.picture)
                                                  : ToneCurves::identity();
    if (area && target.area) {
        // The colour tone of the area after the curves made the target area's
        const RgbColor now = curves.applied(mean_in(rally, *area));
        const RgbColor wanted = mean_in(target.picture, *target.area);
        curves = scaled(curves, gains_towards(now, wanted, false));
    }
    return curves;
}

} // namespace ttrally::cutting
