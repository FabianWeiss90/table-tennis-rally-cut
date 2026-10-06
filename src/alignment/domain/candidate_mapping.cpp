// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/domain/candidate_mapping.hpp"

#include <algorithm>

namespace ttrally::alignment {

namespace {

std::int64_t clamp_frame(std::int64_t frame, std::int64_t frame_count) {
    return std::clamp<std::int64_t>(frame, 0, std::max<std::int64_t>(0, frame_count - 1));
}

/// Collects gaps while walking through the candidates in order.
class GapCollector {
  public:
    explicit GapCollector(double video_start_s) : covered_until_s_(video_start_s) {}

    void add_gap_until(double end_s, std::int64_t next_frame) {
        const std::int64_t start_frame = covered_until_frame_ + 1;
        const std::int64_t end_frame = next_frame - 1;
        if (end_frame < start_frame || end_s <= covered_until_s_) {
            return;
        }
        Gap gap;
        gap.id = static_cast<int>(gaps_.size()) + 1;
        gap.after_candidate = last_candidate_;
        gap.orig_start_s = covered_until_s_;
        gap.orig_end_s = end_s;
        gap.orig_start_frame = start_frame;
        gap.orig_end_frame = end_frame;
        gaps_.push_back(gap);
    }

    void cover(const CandidateSegment& candidate) {
        covered_until_s_ = std::max(covered_until_s_, candidate.orig_end_s);
        covered_until_frame_ = std::max(covered_until_frame_, candidate.orig_end_frame);
        last_candidate_ = candidate.id;
    }

    [[nodiscard]] std::vector<Gap> take() { return std::move(gaps_); }

  private:
    double covered_until_s_;
    std::int64_t covered_until_frame_ = -1;
    int last_candidate_ = 0;
    std::vector<Gap> gaps_;
};

} // namespace

std::vector<CandidateSegment> map_to_candidates(const std::vector<MatchedSegment>& segments,
                                                const media::AudioSignal& original_audio,
                                                const media::AudioSignal& cut_audio,
                                                const OriginalFrames& frames) {
    std::vector<CandidateSegment> candidates;
    candidates.reserve(segments.size());
    for (const MatchedSegment& segment : segments) {
        CandidateSegment candidate;
        candidate.id = static_cast<int>(candidates.size()) + 1;
        candidate.cut_start_s = cut_audio.time_at(static_cast<double>(segment.cut_start));
        candidate.cut_end_s = cut_audio.time_at(static_cast<double>(segment.cut_end));
        candidate.orig_start_s =
            original_audio.time_at(static_cast<double>(segment.original_start()));
        candidate.orig_end_s = original_audio.time_at(static_cast<double>(segment.original_end()));
        candidate.orig_start_frame =
            clamp_frame(frames.timeline.time_to_frame(candidate.orig_start_s), frames.frame_count);
        // The end time is exclusive: the last frame starts before it.
        candidate.orig_end_frame = std::max(
            candidate.orig_start_frame,
            clamp_frame(frames.timeline.time_to_frame(candidate.orig_end_s) - 1,
                        frames.frame_count));
        candidate.offset_s = candidate.orig_start_s - candidate.cut_start_s;
        candidate.confidence = segment.confidence;
        candidate.window_count = segment.window_count;
        candidates.push_back(candidate);
    }
    return candidates;
}

std::vector<Gap> find_gaps(const std::vector<CandidateSegment>& candidates,
                           const OriginalFrames& frames) {
    const FrameTimeline& timeline = frames.timeline;
    const double video_end_s =
        timeline.frame_to_time(frames.frame_count - 1) + 1.0 / timeline.nominal_fps().value();

    GapCollector collector(timeline.start_time());
    for (const CandidateSegment& candidate : candidates) {
        collector.add_gap_until(candidate.orig_start_s, candidate.orig_start_frame);
        collector.cover(candidate);
    }
    collector.add_gap_until(video_end_s, frames.frame_count);
    return collector.take();
}

} // namespace ttrally::alignment
