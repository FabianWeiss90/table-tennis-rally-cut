// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/application/section_planning.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace ttrally::cutting {

namespace {

constexpr double kTimeEpsilon = 1e-6;

/// First frame shown at or after time t (frames: number of frames of the video).
std::int64_t first_frame_from(const FrameTimeline& timeline, double t, std::int64_t frames) {
    std::int64_t frame = std::clamp<std::int64_t>(timeline.time_to_frame(t), 0, frames);
    if (frame < frames && timeline.frame_to_time(frame) < t - kTimeEpsilon) {
        ++frame;
    }
    return frame;
}

/// Moves the section boundaries onto frame times, so that the cut video has no gaps between
/// sections: each section starts with a frame and ends where its last frame ends.
void snap_to_frames(std::vector<CutSection>& sections, const FrameTimeline& timeline,
                    std::int64_t frames, double frame_duration) {
    auto time_of = [&](std::int64_t frame) {
        return frame < frames ? timeline.frame_to_time(frame)
                              : timeline.frame_to_time(frames - 1) + frame_duration;
    };
    for (CutSection& section : sections) {
        section.start_s = time_of(first_frame_from(timeline, section.start_s, frames));
        section.end_s = time_of(first_frame_from(timeline, section.end_s, frames));
    }
    std::erase_if(sections, [](const CutSection& s) { return s.end_s <= s.start_s; });
}

} // namespace

std::vector<CutSection> plan_sections(std::span<const FrameSpan> rallies, Margins margins,
                                      const FrameTimeline& timeline, std::int64_t frame_count) {
    const double frame_duration = 1.0 / timeline.nominal_fps().value();
    std::vector<TimeSpan> spans;
    for (const FrameSpan& rally : rallies) {
        if (rally.start_frame < 0 || rally.end_frame >= frame_count) {
            throw std::invalid_argument(std::format(
                "rally at frames {}-{} lies outside the video (frames 0 to {}); is the cut list "
                "for this video?",
                rally.start_frame, rally.end_frame, frame_count - 1));
        }
        spans.push_back({timeline.frame_to_time(rally.start_frame),
                         timeline.frame_to_time(rally.end_frame) + frame_duration});
    }
    std::vector<CutSection> sections =
        plan_cut(std::move(spans), margins, timeline.frame_to_time(0),
                 timeline.frame_to_time(frame_count - 1) + frame_duration);
    snap_to_frames(sections, timeline, frame_count, frame_duration);
    return sections;
}

} // namespace ttrally::cutting
