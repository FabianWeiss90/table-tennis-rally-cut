// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "alignment/domain/candidates.hpp"
#include "alignment/domain/signal_alignment.hpp"
#include "media/domain/audio_signal.hpp"
#include "shared/kernel/frame_timeline.hpp"

#include <cstdint>
#include <vector>

namespace ttrally::alignment {

/// The original video's frame structure, needed to express times as frame indices.
struct OriginalFrames {
    const FrameTimeline& timeline;
    std::int64_t frame_count = 0;
};

/// Converts matched segments (sample indices) into candidates with times on both files'
/// timelines and frame indices of the original.
[[nodiscard]] std::vector<CandidateSegment>
map_to_candidates(const std::vector<MatchedSegment>& segments,
                  const media::AudioSignal& original_audio, const media::AudioSignal& cut_audio,
                  const OriginalFrames& frames);

/// Parts of the original before, between and after the candidates (at least one frame long).
[[nodiscard]] std::vector<Gap> find_gaps(const std::vector<CandidateSegment>& candidates,
                                         const OriginalFrames& frames);

} // namespace ttrally::alignment
