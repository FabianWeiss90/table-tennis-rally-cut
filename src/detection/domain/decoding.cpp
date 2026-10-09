// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
// Adapted from spin-detector by Yuwei Ba (MIT License)
// https://github.com/ibigbug/spin-detector
// Original files: src/fusion/combine.py (scores_to_segments), src/supervised/common.py
//   (viterbi_decode, viterbi_to_rallies)
// Changes: ported to C++ via training/ttrally_training/decoding.py; parameters in seconds
//   instead of frames at 120 fps; Viterbi with mean rally and pause durations.

#include "detection/domain/decoding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <tuple>
#include <utility>

namespace ttrally::detection {

namespace {

constexpr double kMinProbability = 1e-7; ///< Clipping before the logarithm, as in training
constexpr double kMinLog = 1e-300;

std::vector<RowSegment> threshold_decode(std::span<const float> probabilities, double rate,
                                         const ThresholdDecoding& params) {
    std::vector<bool> above(probabilities.size());
    for (std::size_t row = 0; row < probabilities.size(); ++row) {
        above[row] = static_cast<double>(probabilities[row]) >= params.threshold;
    }
    std::vector<RowSegment> merged;
    const double merge_gap = params.merge_gap_s * rate;
    for (const RowSegment& segment : segments_of(above)) {
        if (!merged.empty() && static_cast<double>(segment.first - merged.back().last) <=
                                   merge_gap) {
            merged.back().last = segment.last;
        } else {
            merged.push_back(segment);
        }
    }
    const double min_length = params.min_rally_s * rate;
    std::erase_if(merged, [&](const RowSegment& s) {
        return static_cast<double>(s.length()) < min_length;
    });
    return merged;
}

double safe_log(double value) { return std::log(std::max(value, kMinLog)); }

std::vector<RowSegment> viterbi_decode(std::span<const float> probabilities, double rate,
                                       const ViterbiDecoding& params) {
    const std::size_t rows = probabilities.size();
    if (rows == 0) {
        return {};
    }
    const double leave_rally = std::min(1.0, 1.0 / (params.mean_rally_s * rate));
    const double leave_pause = std::min(1.0, 1.0 / (params.mean_pause_s * rate));
    const double stay_pause = safe_log(1 - leave_pause);
    const double start_rally = safe_log(leave_pause);
    const double end_rally = safe_log(leave_rally);
    const double stay_rally = safe_log(1 - leave_rally);
    auto emissions = [&](std::size_t t) {
        const double p =
            std::clamp(static_cast<double>(probabilities[t]), kMinProbability, 1 - kMinProbability);
        return std::pair{std::log(1 - p), std::log(p)}; // pause, rally
    };

    // came_from[t][s]: state at t - 1 on the best path into state s at t (0 pause, 1 rally)
    std::vector<std::array<std::uint8_t, 2>> came_from(rows);
    auto [emit_pause, emit_rally] = emissions(0);
    double pause = -std::log(2.0) + emit_pause;
    double rally = -std::log(2.0) + emit_rally;
    for (std::size_t t = 1; t < rows; ++t) {
        std::tie(emit_pause, emit_rally) = emissions(t);
        const double from_pause = pause + stay_pause;
        const double from_rally = rally + end_rally;
        came_from[t][0] = from_pause >= from_rally ? 0 : 1;
        const double new_pause = std::max(from_pause, from_rally) + emit_pause;
        const double to_rally_from_pause = pause + start_rally;
        const double to_rally_from_rally = rally + stay_rally;
        came_from[t][1] = to_rally_from_rally >= to_rally_from_pause ? 1 : 0;
        const double new_rally = std::max(to_rally_from_pause, to_rally_from_rally) + emit_rally;
        pause = new_pause;
        rally = new_rally;
    }

    std::vector<bool> in_rally(rows);
    std::uint8_t state = pause >= rally ? 0 : 1;
    for (std::size_t t = rows; t-- > 0;) {
        in_rally[t] = state == 1;
        state = came_from[t][state];
    }
    return segments_of(in_rally);
}

} // namespace

std::vector<RowSegment> decode(std::span<const float> probabilities, double sample_rate_hz,
                               const DecodingParams& params) {
    if (const auto* threshold = std::get_if<ThresholdDecoding>(&params)) {
        return threshold_decode(probabilities, sample_rate_hz, *threshold);
    }
    return viterbi_decode(probabilities, sample_rate_hz, std::get<ViterbiDecoding>(params));
}

std::string describe(const DecodingParams& params) {
    if (const auto* threshold = std::get_if<ThresholdDecoding>(&params)) {
        return std::format("threshold {:g}, merging gaps up to {:g} s, at least {:g} s",
                           threshold->threshold, threshold->merge_gap_s, threshold->min_rally_s);
    }
    const auto& viterbi = std::get<ViterbiDecoding>(params);
    return std::format("Viterbi, mean rally {:g} s, mean pause {:g} s", viterbi.mean_rally_s,
                       viterbi.mean_pause_s);
}

} // namespace ttrally::detection
