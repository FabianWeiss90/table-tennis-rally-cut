// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/application/cut_video.hpp"

#include "cutting/domain/resolution.hpp"
#include "cutting/application/section_planning.hpp"
#include "media/application/media_error.hpp"

#include <chrono>
#include <format>
#include <stdexcept>

namespace ttrally::cutting {

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

    std::vector<FrameSpan> rallies;
    for (const annotation::RallyLabel& rally : labels.rallies) {
        rallies.push_back({rally.start_frame, rally.end_frame});
    }
    CutVideoResult result;
    result.rallies = rallies.size();
    result.sections = plan_sections(rallies, request.margins, timeline, frames);
    progress_.report(std::format("Cutting {} rallies into {} sections ({:.0f} s of {:.0f} s)...",
                                 result.rallies, result.sections.size(),
                                 total_duration_s(result.sections),
                                 static_cast<double>(frames) * frame_duration));
    result.output = request.output;
    if (request.resolution_in_name) {
        const int max_height =
            request.options.mode == CutMode::Fast ? 0 : request.options.max_height;
        result.output = with_resolution(request.output,
                                        output_height(info.video->height, max_height));
    }
    result.cut = cutter_.cut(request.video, result.sections, result.output, request.options,
                             progress_);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace ttrally::cutting
