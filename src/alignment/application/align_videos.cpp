// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/application/align_videos.hpp"

#include "alignment/domain/alignment_error.hpp"
#include "alignment/domain/audio_aligner.hpp"
#include "alignment/domain/candidate_mapping.hpp"
#include "shared/kernel/version.hpp"

#include <chrono>
#include <cmath>
#include <format>

namespace ttrally::alignment {

struct AlignVideos::Inputs {
    media::MediaInfo original_info;
    media::MediaInfo cut_info;
    media::AudioSignal original_audio;
    media::AudioSignal cut_audio;
    media::VideoTimestamps original_timestamps;
};

namespace {

class Stopwatch {
  public:
    [[nodiscard]] double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

  private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

void require_streams(const media::MediaInfo& original, const media::MediaInfo& cut) {
    if (!original.audio) {
        throw AlignmentNotPossible(original.path.string() + " has no audio stream");
    }
    if (!cut.audio) {
        throw AlignmentNotPossible(cut.path.string() + " has no audio stream");
    }
    if (!original.video) {
        throw AlignmentNotPossible(original.path.string() + " has no video stream");
    }
}

std::vector<WindowPoint> window_points(const SignalAlignment& alignment,
                                       const media::AudioSignal& original,
                                       const media::AudioSignal& cut) {
    std::vector<WindowPoint> points;
    for (const WindowMatch& window : alignment.windows) {
        if (!window.valid) {
            continue;
        }
        const auto start = static_cast<double>(window.cut_index);
        WindowPoint point;
        point.cut_time_s = cut.time_at(start + 0.5 * static_cast<double>(alignment.window_length));
        point.offset_s =
            original.time_at(start + static_cast<double>(window.offset)) - cut.time_at(start);
        point.confidence = window.confidence;
        point.confident = window.confident;
        points.push_back(point);
    }
    return points;
}

/// Frame indices refer to the video timeline; a large audio/video start offset deserves a note.
std::optional<std::string> describe_av_offset(const media::MediaInfo& original, Rational fps) {
    const double offset = original.audio->start_time_s - original.video->start_time_s;
    if (std::abs(offset) <= 0.5 / fps.value()) {
        return std::nullopt;
    }
    return std::format("Audio of the original starts {:+.3f} s relative to its video stream. "
                       "Frame indices use the video timeline.",
                       offset);
}

} // namespace

AlignVideos::AlignVideos(media::MediaProbe& probe, media::MediaReader& reader,
                         media::FrameSamplerFactory& frame_samplers, const SignalMatcher& matcher,
                         ProgressReporter& progress)
    : probe_(probe), reader_(reader), frame_samplers_(frame_samplers), matcher_(matcher),
      progress_(progress) {}

AlignmentReport AlignVideos::execute(const AlignVideosRequest& request) {
    const Stopwatch total;
    const Inputs inputs = load_inputs(request);
    const SignalAlignment alignment = align_audio(inputs, request.settings);

    const media::VideoStreamInfo& video = *inputs.original_info.video;
    const FrameTimeline timeline =
        inputs.original_timestamps.timeline(video.nominal_frame_rate());
    const OriginalFrames frames{timeline, inputs.original_timestamps.frame_count()};

    AlignmentReport report;
    report.tool_version = std::string(version());
    report.original = inputs.original_info;
    report.cut = inputs.cut_info;
    report.original_fps = timeline.nominal_fps();
    report.original_constant_frame_rate = timeline.is_constant();
    report.original_frame_count = frames.frame_count;
    report.settings = request.settings;
    report.statistics = {alignment.windows.size(), alignment.confident_windows,
                         alignment.global_searches};
    report.warnings = alignment.warnings;
    report.windows = window_points(alignment, inputs.original_audio, inputs.cut_audio);
    report.candidates =
        map_to_candidates(alignment.segments, inputs.original_audio, inputs.cut_audio, frames);
    report.gaps = find_gaps(report.candidates, frames);

    if (request.visual_check && !report.candidates.empty()) {
        verify_visually(report, request);
    }
    if (auto note = describe_av_offset(inputs.original_info, report.original_fps)) {
        report.warnings.push_back(*note);
    }
    progress_.report(std::format("Done in {:.1f} s: {} segments, {} gaps, {} warnings.",
                                 total.seconds(), report.candidates.size(), report.gaps.size(),
                                 report.warnings.size()));
    return report;
}

AlignVideos::Inputs AlignVideos::load_inputs(const AlignVideosRequest& request) {
    Inputs inputs;
    inputs.original_info = probe_.probe(request.original);
    inputs.cut_info = probe_.probe(request.cut);
    require_streams(inputs.original_info, inputs.cut_info);

    const int rate = request.settings.sample_rate;
    progress_.report(std::format(
        "Decoding audio (mono, {} Hz) and reading video timestamps...", rate));
    const Stopwatch watch;
    media::MediaContent original = reader_.read(
        request.original, {.audio_sample_rate = rate,
                           .video_stream_index = inputs.original_info.video->index});
    media::MediaContent cut =
        reader_.read(request.cut, {.audio_sample_rate = rate, .video_stream_index = std::nullopt});
    inputs.original_audio = std::move(*original.audio);
    inputs.original_timestamps = std::move(*original.video_timestamps);
    inputs.cut_audio = std::move(*cut.audio);
    if (inputs.original_timestamps.pts.empty()) {
        throw AlignmentNotPossible(request.original.string() +
                                   " has no video frames with timestamps");
    }
    progress_.report(std::format("  original: {:.1f} s audio, {} frames; cut: {:.1f} s audio "
                                 "({:.1f} s)",
                                 inputs.original_audio.duration_s(),
                                 inputs.original_timestamps.frame_count(),
                                 inputs.cut_audio.duration_s(), watch.seconds()));
    return inputs;
}

SignalAlignment AlignVideos::align_audio(const Inputs& inputs, const AlignmentSettings& settings) {
    progress_.report("Aligning audio...");
    const Stopwatch watch;
    const AudioAligner aligner(matcher_);
    SignalAlignment alignment =
        aligner.align(inputs.original_audio.samples, inputs.cut_audio.samples, settings);
    progress_.report(std::format("  {} windows, {} confident, {} global searches ({:.1f} s)",
                                 alignment.windows.size(), alignment.confident_windows,
                                 alignment.global_searches, watch.seconds()));
    if (alignment.confident_fraction() < settings.min_confident_fraction) {
        throw AlignmentNotPossible(std::format(
            "the audio of the cut video could not be matched reliably to the original "
            "({:.1f} % of the windows matched). The audio was probably replaced or altered "
            "(e.g. music overlay); visual alignment is not implemented.",
            100.0 * alignment.confident_fraction()));
    }
    return alignment;
}

void AlignVideos::verify_visually(AlignmentReport& report, const AlignVideosRequest& request) {
    progress_.report("Visual spot check...");
    const Stopwatch watch;
    const VisualCheckPolicy& policy = request.visual_policy;
    try {
        auto original_frames = frame_samplers_.open(request.original, request.decode_backend);
        auto cut_frames = frame_samplers_.open(request.cut, request.decode_backend);
        report.visual_check = policy;
        report.original_decode_backend = original_frames->backend();
        report.cut_decode_backend = cut_frames->backend();

        for (CandidateSegment& candidate : report.candidates) {
            const double middle = candidate.cut_middle_s();
            const auto a = original_frames->sample_gray(middle + candidate.offset_s, policy.width,
                                                        policy.height);
            const auto b = cut_frames->sample_gray(middle, policy.width, policy.height);
            if (a && b) {
                const double similarity = image_similarity(*a, *b);
                if (!std::isnan(similarity)) {
                    candidate.visual_similarity = similarity;
                }
            }
            if (auto warning = assess_visual_check(candidate, policy)) {
                report.warnings.push_back(*warning);
            }
        }
    } catch (const std::exception& error) {
        report.warnings.push_back(std::string("Visual spot check skipped: ") + error.what());
    }
    progress_.report(std::format("  decode backends: original {}, cut {} ({:.1f} s)",
                                 report.original_decode_backend
                                     ? media::to_string(*report.original_decode_backend)
                                     : "-",
                                 report.cut_decode_backend
                                     ? media::to_string(*report.cut_decode_backend)
                                     : "-",
                                 watch.seconds()));
}

} // namespace ttrally::alignment
