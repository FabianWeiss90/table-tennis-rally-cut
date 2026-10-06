// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/audio_aligner.hpp"

#include "alignment/domain/boundary_refinement.hpp"
#include "alignment/domain/consistency_check.hpp"
#include "alignment/domain/segmentation.hpp"
#include "alignment/domain/window_search.hpp"
#include "shared/kernel/parallel.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <stdexcept>

namespace ttrally::alignment {

namespace {

/// Windows matched together. The previous offset used for local searches is only updated
/// between chunks, so the chunk size (not the thread count) determines the results.
constexpr std::size_t kChunkWindows = 64;

/// The early abort check applies only after this many windows and this fraction of all windows.
constexpr std::size_t kEarlyAbortMinWindows = 120;
constexpr double kEarlyAbortMinFraction = 0.25;

std::vector<WindowMatch> create_windows(std::size_t cut_length, std::size_t window_length,
                                        std::size_t hop) {
    const std::size_t count = (cut_length - window_length) / hop + 1;
    std::vector<WindowMatch> windows(count);
    for (std::size_t i = 0; i < count; ++i) {
        windows[i].cut_index = i * hop;
    }
    return windows;
}

bool should_abort_early(std::size_t processed, std::size_t total, std::size_t confident,
                        double min_fraction) {
    const double fraction = static_cast<double>(confident) / static_cast<double>(processed);
    return processed < total && processed >= kEarlyAbortMinWindows &&
           static_cast<double>(processed) >= kEarlyAbortMinFraction * static_cast<double>(total) &&
           fraction < min_fraction;
}

} // namespace

SignalAlignment AudioAligner::align(std::span<const float> original, std::span<const float> cut,
                                    const AlignmentSettings& settings) const {
    settings.validate();
    const std::size_t window_length = settings.window_samples();
    if (cut.size() < window_length) {
        throw std::invalid_argument("the cut audio is shorter than one alignment window");
    }
    if (original.size() < window_length) {
        throw std::invalid_argument("the original audio is shorter than one alignment window");
    }

    SignalAlignment result;
    result.window_length = window_length;
    result.windows = create_windows(cut.size(), window_length, settings.hop_samples());
    if (!match_windows(result, original, cut, settings)) {
        return result;
    }

    result.segments =
        group_into_segments(result.windows, settings.samples(settings.offset_tolerance_s));
    refine_segment_boundaries(result.segments, result.windows, original, cut, settings);
    auto warnings = check_consistency(result.segments, original.size(), settings.sample_rate);
    result.warnings.insert(result.warnings.end(), warnings.begin(), warnings.end());
    return result;
}

bool AudioAligner::match_windows(SignalAlignment& result, std::span<const float> original,
                                 std::span<const float> cut,
                                 const AlignmentSettings& settings) const {
    const WindowSearch search(matcher_, original, cut, settings);
    const unsigned threads = settings.threads != 0 ? settings.threads : default_thread_count();
    auto& windows = result.windows;
    std::optional<std::ptrdiff_t> previous_offset;

    for (std::size_t begin = 0; begin < windows.size(); begin += kChunkWindows) {
        const std::size_t end = std::min(windows.size(), begin + kChunkWindows);
        parallel_for(end - begin, threads,
                     [&](std::size_t j) { search.match(windows[begin + j], previous_offset); });

        for (std::size_t i = begin; i < end; ++i) {
            if (windows[i].confident) {
                ++result.confident_windows;
                previous_offset = windows[i].offset; // the last confident window wins
            }
            if (windows[i].global_search) {
                ++result.global_searches;
            }
        }

        if (should_abort_early(end, windows.size(), result.confident_windows,
                               settings.min_confident_fraction)) {
            result.warnings.push_back(std::format(
                "Alignment stopped after {} of {} windows: only {} matched.", end,
                windows.size(), result.confident_windows));
            windows.resize(end);
            result.aborted_early = true;
            return false;
        }
    }
    return true;
}

} // namespace ttrally::alignment
