// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"

#include "cutting/infrastructure/ffmpeg_encoding.hpp"
#include "media/application/media_error.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <system_error>

namespace ttrally::cutting {

namespace ff = media::ff;
using media::MediaError;

namespace {

constexpr double kTimeEpsilon = 1e-6;
constexpr int kProgressSteps = 10;

// ------------------------------------------------------------------ FFmpeg resources

double seconds(std::int64_t timestamp, AVRational time_base) {
    return timestamp == AV_NOPTS_VALUE ? std::numeric_limits<double>::quiet_NaN()
                                       : static_cast<double>(timestamp) * av_q2d(time_base);
}

std::int64_t timestamp(double seconds, AVRational time_base) {
    return std::llround(seconds / av_q2d(time_base));
}

/// Where input times land in the output: sections follow each other without gaps.
class SectionMap {
  public:
    explicit SectionMap(std::span<const CutSection> sections) : sections_(sections) {
        double offset = 0.0;
        for (const CutSection& section : sections_) {
            offsets_.push_back(offset);
            offset += section.duration_s();
        }
        total_ = offset;
    }

    /// Output time of an input time; nullopt outside every section.
    [[nodiscard]] std::optional<double> output_time(double t) const {
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            if (t >= sections_[i].start_s - kTimeEpsilon && t < sections_[i].end_s - kTimeEpsilon) {
                return offsets_[i] + (t - sections_[i].start_s);
            }
        }
        return std::nullopt;
    }

    /// Parts of [t0, t1) inside sections, as (start, end) pairs, in order.
    [[nodiscard]] std::vector<std::pair<double, double>> parts(double t0, double t1) const {
        std::vector<std::pair<double, double>> result;
        for (const CutSection& section : sections_) {
            const double start = std::max(t0, section.start_s);
            const double end = std::min(t1, section.end_s);
            if (end > start + kTimeEpsilon) {
                result.emplace_back(start, end);
            }
        }
        return result;
    }

    [[nodiscard]] double end() const noexcept { return sections_.back().end_s; }
    [[nodiscard]] double total() const noexcept { return total_; }

  private:
    std::span<const CutSection> sections_;
    std::vector<double> offsets_;
    double total_ = 0.0;
};

int best_stream(AVFormatContext* format, AVMediaType type) {
    const int index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
    return index >= 0 ? index : -1;
}

// ------------------------------------------------------------------ exact cut (re-encoding)

class ExactCut {
  public:
    ExactCut(const std::filesystem::path& input, std::span<const CutSection> sections,
             const std::filesystem::path& output, const CutOptions& options,
             ProgressReporter& progress)
        : format_(ff::open_input(input)), map_(sections), output_(output), progress_(progress) {
        video_index_ = best_stream(format_.get(), AVMEDIA_TYPE_VIDEO);
        if (video_index_ < 0) {
            throw MediaError(input.string() + " has no video stream");
        }
        audio_index_ = best_stream(format_.get(), AVMEDIA_TYPE_AUDIO);
        for (unsigned i = 0; i < format_->nb_streams; ++i) {
            if (static_cast<int>(i) != video_index_ && static_cast<int>(i) != audio_index_) {
                format_->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        video_stream_ = format_->streams[video_index_];
        video_decoder_ = ff::open_decoder(video_stream_);
        video_encoder_.emplace(open_video_encoder(
            options, video_format_of(*video_decoder_, *video_stream_), output_.path(), progress));
        // H.265 keeps the default tag hev1 (parameter sets in the stream). With hvc1, which Apple
        // players prefer, players use the parameter sets of the file header, and those written by
        // the VAAPI encoder do not match its stream (measured: hundreds of decode errors).
        video_out_ = add_encoded_stream(output_, *video_encoder_->context());
        if (audio_index_ >= 0) {
            audio_stream_ = format_->streams[audio_index_];
            audio_decoder_ = ff::open_decoder(audio_stream_);
            audio_encoder_.emplace(audio_decoder_->sample_rate, audio_decoder_->ch_layout,
                                   audio_decoder_->sample_fmt);
            audio_out_ = add_encoded_stream(output_, *audio_encoder_->context());
        }
        output_.open();
    }

    CutResult run() {
        auto packet = ff::make_packet();
        while (!(video_done_ && audio_done())) {
            const int result = av_read_frame(format_.get(), packet.get());
            if (result == AVERROR_EOF) {
                break;
            }
            ff::check(result, "reading the input");
            if (packet->stream_index == video_index_ && !video_done_) {
                decode(video_decoder_.get(), packet.get(), true);
            } else if (packet->stream_index == audio_index_ && !audio_done_) {
                decode(audio_decoder_.get(), packet.get(), false);
            }
            av_packet_unref(packet.get());
        }
        decode(video_decoder_.get(), nullptr, true); // frames still in the decoders
        if (audio_decoder_) {
            decode(audio_decoder_.get(), nullptr, false);
            audio_encoder_->send_ready(true);
            drain(audio_encoder_->context(), audio_out_, output_, true);
        }
        video_encoder_->drain(video_out_, output_, true);
        output_.finish();
        return {video_encoder_->name(),
                static_cast<double>(video_encoder_->context()->bit_rate) / 1e6, map_.total()};
    }

  private:
    [[nodiscard]] bool audio_done() const noexcept { return !audio_decoder_ || audio_done_; }

    /// Sends a packet (nullptr: end of input) and handles every frame the decoder returns.
    void decode(AVCodecContext* decoder, const AVPacket* packet, bool video) {
        for (;;) {
            const int sent = avcodec_send_packet(decoder, packet);
            const bool full = sent == AVERROR(EAGAIN); // collect frames first, then send again
            if (sent < 0 && !full && sent != AVERROR_INVALIDDATA && sent != AVERROR_EOF) {
                ff::check(sent, "decoding");
            }
            receive_all(decoder, video);
            if (!full) {
                return;
            }
        }
    }

    void receive_all(AVCodecContext* decoder, bool video) {
        auto frame = ff::make_frame();
        for (;;) {
            const int result = avcodec_receive_frame(decoder, frame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return;
            }
            ff::check(result, "decoding");
            if (video) {
                handle_video(*frame);
            } else {
                handle_audio(*frame);
            }
            av_frame_unref(frame.get());
        }
    }

    void handle_video(const AVFrame& frame) {
        const double t = seconds(frame.best_effort_timestamp, video_stream_->time_base);
        if (std::isnan(t)) {
            return;
        }
        if (t >= map_.end() - kTimeEpsilon) {
            video_done_ = true;
            return;
        }
        report_progress(t);
        const auto out = map_.output_time(t);
        if (!out) {
            return;
        }
        // Keep timestamps strictly increasing even for slightly irregular input
        const std::int64_t pts = std::max(timestamp(*out, video_encoder_->context()->time_base),
                                          last_video_pts_ + 1);
        last_video_pts_ = pts;
        video_encoder_->send(frame, pts);
        video_encoder_->drain(video_out_, output_, false);
    }

    void handle_audio(const AVFrame& frame) {
        const double t0 = seconds(frame.best_effort_timestamp, audio_stream_->time_base);
        if (std::isnan(t0)) {
            return;
        }
        const double rate = frame.sample_rate;
        const double t1 = t0 + frame.nb_samples / rate;
        if (t0 >= map_.end() - kTimeEpsilon) {
            audio_done_ = true;
            return;
        }
        for (const auto& [start, end] : map_.parts(t0, t1)) {
            const auto first = static_cast<int>(std::lround((start - t0) * rate));
            const auto last = static_cast<int>(std::lround((end - t0) * rate));
            audio_encoder_->append(frame, first, std::min(last, frame.nb_samples) - first);
        }
        audio_encoder_->send_ready(false);
        drain(audio_encoder_->context(), audio_out_, output_, false);
    }

    void report_progress(double t) {
        const double share = std::clamp(t / map_.end(), 0.0, 1.0);
        const int step = static_cast<int>(share * kProgressSteps);
        if (step > reported_step_) {
            reported_step_ = step;
            progress_.report(std::format("  {:3} %", 100 * step / kProgressSteps));
        }
    }

    ff::FormatPtr format_;
    SectionMap map_;
    Output output_;
    ProgressReporter& progress_;
    int video_index_ = -1;
    int audio_index_ = -1;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    ff::CodecPtr video_decoder_;
    ff::CodecPtr audio_decoder_;
    std::optional<VideoEncoder> video_encoder_;
    std::optional<AudioEncoder> audio_encoder_;
    AVStream* video_out_ = nullptr;
    AVStream* audio_out_ = nullptr;
    std::int64_t last_video_pts_ = std::numeric_limits<std::int64_t>::min() / 2;
    bool video_done_ = false;
    bool audio_done_ = false;
    int reported_step_ = 0;
};

// ------------------------------------------------------------------ fast cut (stream copy)

/// Presentation times of the video keyframes, ascending.
std::vector<double> keyframe_times(const std::filesystem::path& input, int video_index) {
    const auto format = ff::open_input(input);
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (static_cast<int>(i) != video_index) {
            format->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    const AVRational time_base = format->streams[video_index]->time_base;
    std::vector<double> times;
    auto packet = ff::make_packet();
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == video_index && (packet->flags & AV_PKT_FLAG_KEY) != 0) {
            times.push_back(seconds(packet->pts, time_base));
        }
        av_packet_unref(packet.get());
    }
    std::erase_if(times, [](double t) { return std::isnan(t); });
    std::ranges::sort(times);
    return times;
}

/// Sections widened to the keyframe at or before their start and the keyframe at or after their
/// end (open end: the end of the video), merged where they now overlap.
std::vector<CutSection> widen_to_keyframes(std::span<const CutSection> sections,
                                           const std::vector<double>& keyframes) {
    std::vector<CutSection> widened;
    for (const CutSection& section : sections) {
        const auto after_start = std::ranges::upper_bound(keyframes, section.start_s + kTimeEpsilon);
        const double start = after_start == keyframes.begin() ? keyframes.front()
                                                              : *std::prev(after_start);
        const auto at_end = std::ranges::lower_bound(keyframes, section.end_s - kTimeEpsilon);
        const double end =
            at_end == keyframes.end() ? std::numeric_limits<double>::infinity() : *at_end;
        if (!widened.empty() && start <= widened.back().end_s) {
            widened.back().end_s = std::max(widened.back().end_s, end);
            widened.back().rallies += section.rallies;
        } else {
            widened.push_back({start, end, section.rallies});
        }
    }
    return widened;
}

CutResult fast_cut(const std::filesystem::path& input, std::span<const CutSection> sections,
                   const std::filesystem::path& output, ProgressReporter& progress) {
    const auto format = ff::open_input(input);
    const int video_index = best_stream(format.get(), AVMEDIA_TYPE_VIDEO);
    if (video_index < 0) {
        throw MediaError(input.string() + " has no video stream");
    }
    const int audio_index = best_stream(format.get(), AVMEDIA_TYPE_AUDIO);
    const std::vector<double> keyframes = keyframe_times(input, video_index);
    if (keyframes.empty()) {
        throw MediaError(input.string() + " has no keyframes");
    }
    const std::vector<CutSection> widened = widen_to_keyframes(sections, keyframes);
    progress.report(std::format("Copying {} sections, widened to keyframes...", widened.size()));

    Output out(output);
    std::vector<AVStream*> out_streams(format->nb_streams, nullptr);
    for (const int index : {video_index, audio_index}) {
        if (index < 0) {
            continue;
        }
        AVStream* stream = out.add_stream();
        ff::check(avcodec_parameters_copy(stream->codecpar, format->streams[index]->codecpar),
                  "stream parameters");
        stream->codecpar->codec_tag = 0;
        stream->time_base = format->streams[index]->time_base;
        out_streams[static_cast<std::size_t>(index)] = stream;
    }
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        if (out_streams[i] == nullptr) {
            format->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    out.open();

    std::vector<double> offsets;
    double offset = 0.0;
    for (const CutSection& section : widened) {
        offsets.push_back(offset);
        offset += std::isinf(section.end_s) ? 0.0 : section.duration_s();
    }
    std::vector<std::int64_t> last_dts(format->nb_streams, std::numeric_limits<std::int64_t>::min());
    double written_end = 0.0;
    auto packet = ff::make_packet();
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        const auto index = static_cast<std::size_t>(packet->stream_index);
        AVStream* in_stream = format->streams[index];
        AVStream* out_stream = index < out_streams.size() ? out_streams[index] : nullptr;
        const double t = seconds(packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts,
                                 in_stream->time_base);
        if (out_stream == nullptr || std::isnan(t)) {
            av_packet_unref(packet.get());
            continue;
        }
        // Keyframe boundaries make each section start with a keyframe and end before one.
        const auto section = std::ranges::find_if(widened, [t](const CutSection& s) {
            return t >= s.start_s - kTimeEpsilon && t < s.end_s - kTimeEpsilon;
        });
        if (section == widened.end()) {
            av_packet_unref(packet.get());
            continue;
        }
        const auto i = static_cast<std::size_t>(section - widened.begin());
        const std::int64_t shift = timestamp(offsets[i] - section->start_s, in_stream->time_base);
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts += shift;
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            packet->dts += shift;
            packet->dts = std::max(packet->dts, last_dts[index] + 1);
            if (packet->pts != AV_NOPTS_VALUE) {
                packet->pts = std::max(packet->pts, packet->dts);
            }
            last_dts[index] = packet->dts;
        }
        written_end = std::max(written_end, offsets[i] + (t - section->start_s) +
                                                seconds(packet->duration, in_stream->time_base));
        av_packet_rescale_ts(packet.get(), in_stream->time_base, out_stream->time_base);
        packet->stream_index = out_stream->index;
        packet->pos = -1;
        out.write(*packet);
    }
    out.finish();
    return {"copy", 0.0, written_end};
}

} // namespace

CutResult FfmpegVideoCutter::cut(const std::filesystem::path& input,
                                 std::span<const CutSection> sections,
                                 const std::filesystem::path& output, const CutOptions& options,
                                 ProgressReporter& progress) {
    if (sections.empty()) {
        throw std::invalid_argument("nothing to cut");
    }
    try {
        if (options.mode == CutMode::Fast) {
            return fast_cut(input, sections, output, progress);
        }
        return ExactCut(input, sections, output, options, progress).run();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(output, ignored); // never leave a half-written video
        throw;
    }
}

} // namespace ttrally::cutting
