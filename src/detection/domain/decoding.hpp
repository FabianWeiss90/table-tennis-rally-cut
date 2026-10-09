// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 AND MIT
// Adapted from spin-detector by Yuwei Ba (MIT License)
// https://github.com/ibigbug/spin-detector
// Original files: src/fusion/combine.py (scores_to_segments), src/supervised/common.py
//   (viterbi_decode, viterbi_to_rallies)
// Changes: ported to C++ via training/ttrally_training/decoding.py; parameters in seconds
//   instead of frames at 120 fps; Viterbi with mean rally and pause durations.

#pragma once

#include "detection/domain/row_segment.hpp"

#include <span>
#include <string>
#include <variant>
#include <vector>

namespace ttrally::detection {

/// Rows at or above the threshold; segments separated by at most merge_gap_s are merged, then
/// segments shorter than min_rally_s are dropped.
struct ThresholdDecoding {
    double threshold = 0.5;
    double merge_gap_s = 1.0;
    double min_rally_s = 0.5;
};

/// Most likely rally / pause sequence of a two-state hidden Markov model whose states last
/// mean_rally_s and mean_pause_s on average; the rally probabilities are the emissions.
struct ViterbiDecoding {
    double mean_rally_s = 6.0;
    double mean_pause_s = 20.0;
};

/// How probabilities become rallies; chosen in training and stored in the detector model.
using DecodingParams = std::variant<ThresholdDecoding, ViterbiDecoding>;

/// Rally segments of per-row probabilities sampled at sample_rate_hz rows per second. Must give
/// exactly the result of decode() in training/ttrally_training/decoding.py.
[[nodiscard]] std::vector<RowSegment> decode(std::span<const float> probabilities,
                                             double sample_rate_hz, const DecodingParams& params);

/// e.g. "threshold 0.55, merge gaps up to 1 s, min. 0.5 s"
[[nodiscard]] std::string describe(const DecodingParams& params);

} // namespace ttrally::detection
