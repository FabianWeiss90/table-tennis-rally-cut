// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Synthetic audio helpers for tests that must run without media files.

#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace ttrally::test {

/// Noise with interspersed impulses (short decaying bursts, like ball contacts).
inline std::vector<float> noise_with_impulses(std::size_t samples, double rate,
                                              std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0F, 1.0F);
    std::uniform_real_distribution<double> gap(0.2, 1.5);
    std::uniform_real_distribution<double> pitch(800.0, 3000.0);

    std::vector<float> signal(samples);
    float low = 0.0F;
    for (auto& value : signal) {
        // Mix of white and low-passed noise, roughly like room noise
        const float white = noise(rng);
        low = 0.98F * low + 0.02F * white;
        value = 0.05F * white + 0.6F * low;
    }
    for (double t = gap(rng); t < static_cast<double>(samples) / rate; t += gap(rng)) {
        const double f = pitch(rng);
        const auto start = static_cast<std::size_t>(t * rate);
        const auto length = static_cast<std::size_t>(0.03 * rate);
        for (std::size_t i = 0; i < length && start + i < samples; ++i) {
            const double time = static_cast<double>(i) / rate;
            signal[start + i] += static_cast<float>(
                0.8 * std::exp(-time / 0.006) * std::sin(2.0 * std::numbers::pi * f * time));
        }
    }
    return signal;
}

/// Band-limited resampling with a Hann-windowed sinc kernel (table lookup).
inline std::vector<float> resample(std::span<const float> input, double rate_in, double rate_out) {
    constexpr int kZeroCrossings = 8;
    constexpr int kTableResolution = 512;
    const double ratio = rate_out / rate_in;
    const double cutoff = 0.95 * std::min(1.0, ratio); // relative to the input Nyquist frequency
    const double half_width = kZeroCrossings / cutoff;  // in input samples

    const auto table_size = static_cast<std::size_t>(std::ceil(half_width * kTableResolution)) + 2;
    std::vector<double> table(table_size);
    for (std::size_t i = 0; i < table_size; ++i) {
        const double d = static_cast<double>(i) / kTableResolution;
        if (d >= half_width) {
            table[i] = 0.0;
            continue;
        }
        const double x = std::numbers::pi * cutoff * d;
        const double sinc = d == 0.0 ? 1.0 : std::sin(x) / x;
        const double window = 0.5 * (1.0 + std::cos(std::numbers::pi * d / half_width));
        table[i] = cutoff * sinc * window;
    }
    auto kernel = [&](double d) {
        const double position = std::abs(d) * kTableResolution;
        const auto index = static_cast<std::size_t>(position);
        if (index + 1 >= table_size) {
            return 0.0;
        }
        const double frac = position - static_cast<double>(index);
        return table[index] + frac * (table[index + 1] - table[index]);
    };

    const auto output_size = static_cast<std::size_t>(static_cast<double>(input.size()) * ratio);
    std::vector<float> output(output_size);
    const auto last = static_cast<std::ptrdiff_t>(input.size()) - 1;
    for (std::size_t j = 0; j < output_size; ++j) {
        const double x = static_cast<double>(j) / ratio;
        const auto first =
            std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(std::ceil(x - half_width)));
        const auto end =
            std::min<std::ptrdiff_t>(last, static_cast<std::ptrdiff_t>(std::floor(x + half_width)));
        double sum = 0.0;
        for (std::ptrdiff_t i = first; i <= end; ++i) {
            sum += input[static_cast<std::size_t>(i)] * kernel(x - static_cast<double>(i));
        }
        output[j] = static_cast<float>(sum);
    }
    return output;
}

/// A part of the original that appears in the cut.
struct Piece {
    double orig_start;
    double orig_end;
    [[nodiscard]] double duration() const { return orig_end - orig_start; }
};

/// An original and a cut made from it, both at 8 kHz, perturbed like a re-encoded video:
/// unrelated intro, the pieces with 10 ms fades and a gain change, resampling
/// 48 kHz -> 44.1 kHz -> 8 kHz, and added noise (about -35 dB).
struct CutScenario {
    static constexpr int kRate = 8000;
    std::vector<float> original;
    std::vector<float> cut;
    double intro_s = 0.0;
    std::vector<Piece> pieces;

    /// Expected position of piece k in the cut (seconds from the start of the cut).
    [[nodiscard]] double cut_start(std::size_t k) const {
        double position = intro_s;
        for (std::size_t i = 0; i < k; ++i) {
            position += pieces[i].duration();
        }
        return position;
    }
};

inline CutScenario make_cut_scenario(std::vector<Piece> pieces, double original_s, double intro_s,
                                     std::uint32_t seed) {
    constexpr double kSourceRate = 48000.0;
    constexpr double kGain = 0.7;
    constexpr double kFadeSeconds = 0.010;
    const auto source = noise_with_impulses(static_cast<std::size_t>(original_s * kSourceRate),
                                            kSourceRate, seed);

    std::vector<float> cut48 = noise_with_impulses(
        static_cast<std::size_t>(intro_s * kSourceRate), kSourceRate, seed + 1);
    const auto fade = static_cast<std::size_t>(kFadeSeconds * kSourceRate);
    for (const auto& piece : pieces) {
        const auto begin = static_cast<std::size_t>(std::llround(piece.orig_start * kSourceRate));
        const auto end = static_cast<std::size_t>(std::llround(piece.orig_end * kSourceRate));
        for (std::size_t i = begin; i < end; ++i) {
            const std::size_t from_start = i - begin;
            const std::size_t to_end = end - 1 - i;
            double gain = kGain;
            if (from_start < fade) {
                gain *= static_cast<double>(from_start) / static_cast<double>(fade);
            }
            if (to_end < fade) {
                gain *= static_cast<double>(to_end) / static_cast<double>(fade);
            }
            cut48.push_back(static_cast<float>(gain * source[i]));
        }
    }

    CutScenario scenario;
    scenario.original = resample(source, kSourceRate, CutScenario::kRate);
    scenario.cut = resample(resample(cut48, kSourceRate, 44100.0), 44100.0, CutScenario::kRate);
    std::mt19937 rng(seed + 2);
    std::normal_distribution<float> noise(0.0F, 0.004F);
    for (auto& value : scenario.cut) {
        value += noise(rng);
    }
    scenario.intro_s = intro_s;
    scenario.pieces = std::move(pieces);
    return scenario;
}

} // namespace ttrally::test
