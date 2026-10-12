// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/application/cut_video.hpp"

#include "media/application/media_error.hpp"

#include <algorithm>
#include <chrono>
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

CutVideo::CutVideo(media::MediaProbe& probe, media::MediaReader& reader,
                   annotation::AnnotationRepository& cut_list, VideoCutter& cutter,
                   ProgressReporter& progress)
    : probe_(probe), reader_(reader), cut_list_(cut_list), cutter_(cutter), progress_(progress) {}

CutVideoResult CutVideo::execute(const CutVideoRequest& request) {
    const auto started = std::chrono::steady_clock::now();
    const annotation::StoredLabels labels = cut_list_.load(request.video_id);
    if (labels.rallies.empty()) {
        throw std::invalid_argument("the cut list has no rallies for " + request.video_id);
    }
    const media::MediaInfo info = probe_.probe(request.video);
    if (!info.video) {
        throw media::MediaError(request.video.string() + " has no video stream");
    }
    const auto timestamps =
        *reader_
             .read(request.video,
                   {.audio_sample_rate = std::nullopt, .video_stream_index = info.video->index})
             .video_timestamps;
    const FrameTimeline timeline = timestamps.timeline(info.video->nominal_frame_rate());
    const std::int64_t frames = timestamps.frame_count();
    const double frame_duration = 1.0 / timeline.nominal_fps().value();

    std::vector<TimeSpan> rallies;
    for (const annotation::RallyLabel& rally : labels.rallies) {
        if (rally.start_frame < 0 || rally.end_frame >= frames) {
            throw std::invalid_argument(std::format(
                "rally at frames {}-{} lies outside the video (frames 0 to {}); is the cut list "
                "for this video?",
                rally.start_frame, rally.end_frame, frames - 1));
        }
        rallies.push_back({timeline.frame_to_time(rally.start_frame),
                           timeline.frame_to_time(rally.end_frame) + frame_duration});
    }
    CutVideoResult result;
    result.rallies = rallies.size();
    result.sections = plan_cut(std::move(rallies), request.margins, timeline.frame_to_time(0),
                               timeline.frame_to_time(frames - 1) + frame_duration);
    snap_to_frames(result.sections, timeline, frames, frame_duration);
    progress_.report(std::format("Cutting {} rallies into {} sections ({:.0f} s of {:.0f} s)...",
                                 result.rallies, result.sections.size(),
                                 total_duration_s(result.sections),
                                 static_cast<double>(frames) * frame_duration));
    result.cut = cutter_.cut(request.video, result.sections, request.output, request.options,
                             progress_);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace ttrally::cutting
