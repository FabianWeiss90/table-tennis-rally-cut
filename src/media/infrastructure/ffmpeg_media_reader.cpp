// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_media_reader.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace ttrally::media {

namespace {

/// Decodes one audio stream and converts it to mono float at a fixed sample rate.
class MonoAudioDecoder {
  public:
    MonoAudioDecoder(const AVStream* stream, int sample_rate, std::string source)
        : stream_(stream), decoder_(ff::open_decoder(stream)), frame_(ff::make_frame()),
          source_(std::move(source)) {
        signal_.sample_rate = sample_rate;
        signal_.start_time_s = std::numeric_limits<double>::quiet_NaN();
    }

    MonoAudioDecoder(const MonoAudioDecoder&) = delete;
    MonoAudioDecoder& operator=(const MonoAudioDecoder&) = delete;
    ~MonoAudioDecoder() { av_channel_layout_uninit(&input_layout_); }

    void decode(const AVPacket* packet) {
        const int sent = avcodec_send_packet(decoder_.get(), packet);
        // Corrupt packets are skipped; the decoder resynchronises on the next one.
        if (sent < 0 && sent != AVERROR_INVALIDDATA) {
            ff::check(sent, "audio decoding of " + source_);
        }
        receive_frames();
    }

    [[nodiscard]] AudioSignal finish() {
        ff::check(avcodec_send_packet(decoder_.get(), nullptr), "audio decoder flush");
        receive_frames();
        resample(nullptr);
        if (std::isnan(signal_.start_time_s)) {
            const double stream_start = ff::to_seconds(stream_->start_time, stream_->time_base);
            signal_.start_time_s = std::isnan(stream_start) ? 0.0 : stream_start;
        }
        if (signal_.samples.empty()) {
            throw MediaError("no audio samples could be decoded from " + source_);
        }
        return std::move(signal_);
    }

  private:
    void receive_frames() {
        for (;;) {
            const int received = avcodec_receive_frame(decoder_.get(), frame_.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                return;
            }
            ff::check(received, "audio decoding of " + source_);
            if (std::isnan(signal_.start_time_s)) {
                signal_.start_time_s =
                    ff::to_seconds(frame_->best_effort_timestamp, stream_->time_base);
            }
            configure_resampler(frame_.get());
            resample(frame_.get());
            av_frame_unref(frame_.get());
        }
    }

    /// (Re)creates the resampler when the input format changes.
    void configure_resampler(const AVFrame* frame) {
        if (resampler_ && frame->format == input_format_ && frame->sample_rate == input_rate_ &&
            av_channel_layout_compare(&frame->ch_layout, &input_layout_) == 0) {
            return;
        }
        resample(nullptr); // flush samples buffered with the previous configuration
        resampler_.reset();
        av_channel_layout_uninit(&input_layout_);

        SwrContext* raw = nullptr;
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        ff::check(swr_alloc_set_opts2(&raw, &mono, AV_SAMPLE_FMT_FLT, signal_.sample_rate,
                                      &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
                                      frame->sample_rate, 0, nullptr),
                  "audio resampler setup");
        resampler_.reset(raw);
        ff::check(swr_init(resampler_.get()), "audio resampler init");
        ff::check(av_channel_layout_copy(&input_layout_, &frame->ch_layout), "channel layout");
        input_format_ = frame->format;
        input_rate_ = frame->sample_rate;
    }

    /// Appends the resampled frame to the signal; nullptr flushes the resampler.
    void resample(const AVFrame* frame) {
        if (!resampler_) {
            return;
        }
        const int input_samples = frame != nullptr ? frame->nb_samples : 0;
        const int capacity = swr_get_out_samples(resampler_.get(), input_samples);
        if (capacity <= 0) {
            return;
        }
        auto& samples = signal_.samples;
        const std::size_t old_size = samples.size();
        samples.resize(old_size + static_cast<std::size_t>(capacity));
        auto* output = reinterpret_cast<std::uint8_t*>(samples.data() + old_size);
        const auto** input =
            frame != nullptr ? const_cast<const std::uint8_t**>(frame->extended_data) : nullptr;
        const int converted =
            swr_convert(resampler_.get(), &output, capacity, input, input_samples);
        ff::check(converted, "audio resampling");
        samples.resize(old_size + static_cast<std::size_t>(converted));
    }

    const AVStream* stream_;
    ff::CodecPtr decoder_;
    ff::FramePtr frame_;
    ff::SwrPtr resampler_;
    AVChannelLayout input_layout_{};
    int input_format_ = -1;
    int input_rate_ = 0;
    AudioSignal signal_;
    std::string source_;
};

/// Index of the audio stream to decode, or -1 if no audio was requested.
int select_audio_stream(AVFormatContext* format, const ReadRequest& request,
                        const std::filesystem::path& path) {
    if (!request.audio_sample_rate) {
        return -1;
    }
    const int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        throw MediaError(path.string() + " has no audio stream");
    }
    return index;
}

void validate_video_stream(const AVFormatContext* format, const ReadRequest& request,
                           const std::filesystem::path& path) {
    if (!request.video_stream_index) {
        return;
    }
    const int index = *request.video_stream_index;
    if (index < 0 || index >= static_cast<int>(format->nb_streams) ||
        format->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        throw MediaError("invalid video stream index for " + path.string());
    }
}

/// Lets the demuxer skip all streams that are not needed.
void discard_other_streams(AVFormatContext* format, int audio_index,
                           const std::optional<int>& video_index) {
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const auto index = static_cast<int>(i);
        if (index != audio_index && index != video_index.value_or(-1)) {
            format->streams[i]->discard = AVDISCARD_ALL;
        }
    }
}

bool is_presented(const AVPacket* packet) {
    return packet->pts != AV_NOPTS_VALUE && (packet->flags & AV_PKT_FLAG_DISCARD) == 0;
}

} // namespace

MediaContent FfmpegMediaReader::read(const std::filesystem::path& path,
                                     const ReadRequest& request) {
    auto format = ff::open_input(path);
    const int audio_index = select_audio_stream(format.get(), request, path);
    validate_video_stream(format.get(), request, path);
    discard_other_streams(format.get(), audio_index, request.video_stream_index);

    std::optional<MonoAudioDecoder> audio;
    if (audio_index >= 0) {
        audio.emplace(format->streams[audio_index], *request.audio_sample_rate, path.string());
    }
    std::vector<std::int64_t> pts;
    const int video_index = request.video_stream_index.value_or(-1);

    auto packet = ff::make_packet();
    while (av_read_frame(format.get(), packet.get()) >= 0) {
        if (packet->stream_index == video_index && is_presented(packet.get())) {
            pts.push_back(packet->pts);
        } else if (packet->stream_index == audio_index) {
            audio->decode(packet.get());
        }
        av_packet_unref(packet.get());
    }

    MediaContent content;
    if (audio) {
        content.audio = audio->finish();
    }
    if (request.video_stream_index) {
        std::sort(pts.begin(), pts.end());
        const AVRational time_base = format->streams[video_index]->time_base;
        content.video_timestamps = VideoTimestamps{std::move(pts), {time_base.num, time_base.den}};
    }
    return content;
}

} // namespace ttrally::media
