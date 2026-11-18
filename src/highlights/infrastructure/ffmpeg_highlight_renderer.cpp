// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/ffmpeg_highlight_renderer.hpp"

#include "cutting/infrastructure/ffmpeg_encoding.hpp"
#include "highlights/infrastructure/title_painter.hpp"
#include "media/application/media_error.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ttrally::highlights {

namespace ff = media::ff;
using cutting::VideoFormat;
using media::MediaError;

namespace {

constexpr int kAudioRate = 48'000;
constexpr int kAudioChannels = 2;
constexpr double kTimeEpsilon = 1e-6;
constexpr int kWeightOne = 256; ///< fixed-point 1.0 for blending pictures

using AudioPlanes = std::array<std::vector<float>, kAudioChannels>;

int best_stream(AVFormatContext* format, AVMediaType type) {
    const int index = av_find_best_stream(format, type, -1, -1, nullptr, 0);
    return index >= 0 ? index : -1;
}

/// Output frame n of a clip shows the clip's time start + n / rate.
double frame_time(std::int64_t n, AVRational rate) {
    return static_cast<double>(n) * av_q2d(av_inv_q(rate));
}

/// Output sample at output frame n (rounded, so that sound and picture stay in step).
std::int64_t sample_at_frame(std::int64_t n, AVRational rate) {
    return av_rescale_q_rnd(n, av_inv_q(rate), {1, kAudioRate},
                            static_cast<AVRounding>(AV_ROUND_NEAR_INF));
}

/// Where a clip sits in the output, in output frames.
struct ClipTiming {
    std::int64_t first = 0;    ///< output frame of the clip's first frame
    std::int64_t frames = 0;   ///< frames of the clip
    std::int64_t fade_in = 0;  ///< first frames shared with the previous clip
    std::int64_t fade_out = 0; ///< last frames shared with the next clip

    [[nodiscard]] std::int64_t end() const noexcept { return first + frames; }
};

std::vector<ClipTiming> plan_timing(std::span<const Clip> clips, AVRational rate,
                                    double crossfade_s) {
    std::vector<ClipTiming> timing;
    for (const Clip& clip : clips) {
        timing.push_back({0,
                          std::max<std::int64_t>(
                              1, std::llround((clip.end_s - clip.start_s) * av_q2d(rate))),
                          0, 0});
    }
    const auto wanted = static_cast<std::int64_t>(std::llround(crossfade_s * av_q2d(rate)));
    for (std::size_t i = 0; i + 1 < timing.size(); ++i) {
        // At most half of each clip, so that a clip's fade-in and fade-out never overlap
        const std::int64_t shared =
            std::min({wanted, timing[i].frames / 2, timing[i + 1].frames / 2});
        timing[i].fade_out = shared;
        timing[i + 1].fade_in = shared;
        timing[i + 1].first = timing[i].end() - shared;
    }
    return timing;
}

ff::FramePtr make_picture(const VideoFormat& format) {
    auto picture = ff::make_frame();
    picture->format = AV_PIX_FMT_YUV420P;
    picture->width = format.width;
    picture->height = format.height;
    picture->color_range = format.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    picture->colorspace = format.colorspace;
    picture->color_primaries = format.color_primaries;
    picture->color_trc = format.color_trc;
    ff::check(av_frame_get_buffer(picture.get(), 0), "frame buffer");
    return picture;
}

/// into = into * (1 - share) + from * share, share = weight / kWeightOne.
void blend(AVFrame& into, const AVFrame& from, int weight) {
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? into.width : (into.width + 1) / 2;
        const int height = plane == 0 ? into.height : (into.height + 1) / 2;
        for (int y = 0; y < height; ++y) {
            std::uint8_t* a =
                into.data[plane] + static_cast<std::ptrdiff_t>(y) * into.linesize[plane];
            const std::uint8_t* b =
                from.data[plane] + static_cast<std::ptrdiff_t>(y) * from.linesize[plane];
            for (int x = 0; x < width; ++x) {
                a[x] = static_cast<std::uint8_t>(
                    (a[x] * (kWeightOne - weight) + b[x] * weight + kWeightOne / 2) / kWeightOne);
            }
        }
    }
}

/// Fonts are loaded and titles rendered once, when first needed.
class TitleCache {
  public:
    explicit TitleCache(std::filesystem::path font) : font_file_(std::move(font)) {}

    const TitleMask& mask(const Title& title, int picture_height) {
        const int line_height =
            std::max(1, static_cast<int>(std::lround(title.size * picture_height)));
        const auto key = std::make_tuple(title.text, line_height, title.position);
        if (const auto found = masks_.find(key); found != masks_.end()) {
            return found->second;
        }
        if (!font_) {
            if (font_file_.empty()) {
                font_file_ = default_title_font();
            }
            if (font_file_.empty()) {
                throw std::runtime_error(
                    "no font found for the titles: give a TrueType font file with --font");
            }
            font_ = std::make_unique<TitleFont>(font_file_);
        }
        return masks_
            .emplace(key, font_->render(title.text, line_height, align_for(title.position)))
            .first->second;
    }

  private:
    std::filesystem::path font_file_;
    std::unique_ptr<TitleFont> font_;
    std::map<std::tuple<std::string, int, TitlePosition>, TitleMask> masks_;
};

/// Decodes one clip and hands out its pictures on the output's frame grid and its sound.
class ClipReader {
  public:
    using PictureConsumer = std::function<void(std::int64_t, AVFrame&)>;

    ClipReader(const Clip& clip, const VideoFormat& output, std::int64_t frames)
        : clip_(clip), output_(output), frames_(frames), format_(ff::open_input(clip.video)) {
        video_index_ = best_stream(format_.get(), AVMEDIA_TYPE_VIDEO);
        if (video_index_ < 0) {
            throw MediaError(clip.video.string() + " has no video stream");
        }
        audio_index_ = best_stream(format_.get(), AVMEDIA_TYPE_AUDIO);
        for (unsigned i = 0; i < format_->nb_streams; ++i) {
            if (static_cast<int>(i) != video_index_ && static_cast<int>(i) != audio_index_) {
                format_->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        video_stream_ = format_->streams[video_index_];
        video_decoder_ = ff::open_decoder(video_stream_);
        if (audio_index_ >= 0) {
            audio_stream_ = format_->streams[audio_index_];
            audio_decoder_ = ff::open_decoder(audio_stream_);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            SwrContext* resampler = nullptr;
            ff::check(swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_FLTP, kAudioRate,
                                          &audio_decoder_->ch_layout, audio_decoder_->sample_fmt,
                                          audio_decoder_->sample_rate, 0, nullptr),
                      "audio conversion");
            resampler_.reset(resampler);
            ff::check(swr_init(resampler_.get()), "audio conversion");
        }
    }

    /// Passes the clip's pictures (output frames 0..frames-1, in output format, before any
    /// title) to `consume` and writes its sound, from the clip's start on, into `audio`
    /// (planar stereo; its size is kept, missing sound stays silent).
    void read(const PictureConsumer& consume, AudioPlanes& audio) {
        consume_ = &consume;
        audio_ = &audio;
        seek();
        auto packet = ff::make_packet();
        while (!(next_ >= frames_ && audio_done())) {
            const int result = av_read_frame(format_.get(), packet.get());
            if (result == AVERROR_EOF) {
                break;
            }
            ff::check(result, "reading " + clip_.video.string());
            if (packet->stream_index == video_index_ && next_ < frames_) {
                decode(video_decoder_.get(), packet.get(), true);
            } else if (packet->stream_index == audio_index_ && !audio_done_) {
                decode(audio_decoder_.get(), packet.get(), false);
            }
            av_packet_unref(packet.get());
        }
        if (next_ < frames_) {
            decode(video_decoder_.get(), nullptr, true);
        }
        while (next_ < frames_ && last_) { // the video ended early: hold its last picture
            emit_last();
        }
        if (next_ < frames_) {
            throw MediaError(std::format("{}: no picture at {:.2f} s", clip_.video.string(),
                                         clip_.start_s));
        }
    }

  private:
    [[nodiscard]] bool audio_done() const noexcept { return !audio_decoder_ || audio_done_; }

    void seek() {
        const auto target = static_cast<std::int64_t>(
            std::floor(clip_.start_s / av_q2d(video_stream_->time_base)));
        // A failed seek (e.g. a file without index) reads from the start, which is only slower
        av_seek_frame(format_.get(), video_index_, target, AVSEEK_FLAG_BACKWARD);
    }

    void decode(AVCodecContext* decoder, const AVPacket* packet, bool video) {
        for (;;) {
            const int sent = avcodec_send_packet(decoder, packet);
            const bool full = sent == AVERROR(EAGAIN);
            if (sent < 0 && !full && sent != AVERROR_INVALIDDATA && sent != AVERROR_EOF) {
                ff::check(sent, "decoding");
            }
            auto frame = ff::make_frame();
            for (;;) {
                const int result = avcodec_receive_frame(decoder, frame.get());
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                    break;
                }
                ff::check(result, "decoding");
                if (video) {
                    take_picture(std::move(frame));
                    frame = ff::make_frame();
                } else {
                    take_sound(*frame);
                    av_frame_unref(frame.get());
                }
            }
            if (!full) {
                return;
            }
        }
    }

    /// A decoded picture is shown from its time until the next one's: every output frame
    /// before it gets the previous picture.
    void take_picture(ff::FramePtr frame) {
        const double t = ff::to_seconds(frame->best_effort_timestamp, video_stream_->time_base);
        if (std::isnan(t)) {
            return;
        }
        while (next_ < frames_ && clip_.start_s + frame_time(next_, output_.frame_rate) <
                                      t - kTimeEpsilon) {
            if (!last_) {
                break; // the clip starts before the video's first picture: use this one
            }
            emit_last();
        }
        last_ = std::move(frame);
        converted_.reset();
    }

    void emit_last() {
        if (!converted_) {
            converted_ = convert(*last_);
        }
        // The consumer may paint on it, so it gets its own copy
        auto picture = make_picture(output_);
        ff::check(av_frame_copy(picture.get(), converted_.get()), "frame copy");
        (*consume_)(next_++, *picture);
    }

    ff::FramePtr convert(const AVFrame& decoded) {
        auto picture = make_picture(output_);
        scaler_.reset(sws_getCachedContext(scaler_.release(), decoded.width, decoded.height,
                                           static_cast<AVPixelFormat>(decoded.format),
                                           output_.width, output_.height, AV_PIX_FMT_YUV420P,
                                           SWS_BICUBIC, nullptr, nullptr, nullptr));
        if (!scaler_) {
            throw MediaError("cannot convert the pictures of " + clip_.video.string());
        }
        const bool source_full = decoded.color_range == AVCOL_RANGE_JPEG ||
                                 decoded.format == AV_PIX_FMT_YUVJ420P ||
                                 decoded.format == AV_PIX_FMT_YUVJ422P ||
                                 decoded.format == AV_PIX_FMT_YUVJ444P;
        sws_setColorspaceDetails(scaler_.get(), sws_getCoefficients(decoded.colorspace),
                                 source_full ? 1 : 0, sws_getCoefficients(output_.colorspace),
                                 output_.full_range ? 1 : 0, 0, 1 << 16, 1 << 16);
        sws_scale(scaler_.get(), decoded.data, decoded.linesize, 0, decoded.height,
                  picture->data, picture->linesize);
        return picture;
    }

    void take_sound(const AVFrame& frame) {
        const double t = ff::to_seconds(frame.best_effort_timestamp, audio_stream_->time_base);
        if (std::isnan(t)) {
            return;
        }
        const double clip_end = clip_.start_s + frame_time(frames_, output_.frame_rate);
        if (t >= clip_end) {
            audio_done_ = true;
            return;
        }
        auto converted = ff::make_frame();
        converted->format = AV_SAMPLE_FMT_FLTP;
        converted->sample_rate = kAudioRate;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        ff::check(av_channel_layout_copy(&converted->ch_layout, &stereo), "channel layout");
        ff::check(swr_convert_frame(resampler_.get(), converted.get(), &frame), "audio conversion");
        const auto first =
            static_cast<std::int64_t>(std::llround((t - clip_.start_s) * kAudioRate));
        const auto size = static_cast<std::int64_t>((*audio_)[0].size());
        for (int c = 0; c < kAudioChannels; ++c) {
            const auto* samples = reinterpret_cast<const float*>(converted->extended_data[c]);
            std::vector<float>& channel = (*audio_)[static_cast<std::size_t>(c)];
            for (int k = 0; k < converted->nb_samples; ++k) {
                const std::int64_t at = first + k;
                if (at >= 0 && at < size) {
                    channel[static_cast<std::size_t>(at)] = samples[k];
                }
            }
        }
    }

    const Clip& clip_;
    const VideoFormat& output_;
    std::int64_t frames_;
    ff::FormatPtr format_;
    int video_index_ = -1;
    int audio_index_ = -1;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    ff::CodecPtr video_decoder_;
    ff::CodecPtr audio_decoder_;
    ff::SwrPtr resampler_;
    ff::SwsPtr scaler_;
    const PictureConsumer* consume_ = nullptr;
    AudioPlanes* audio_ = nullptr;
    ff::FramePtr last_;      ///< latest decoded picture
    ff::FramePtr converted_; ///< it in the output format, once needed
    std::int64_t next_ = 0;  ///< next output frame of the clip
    bool audio_done_ = false;
};

/// The output side: encoders, the pictures held for a cross-fade and the sound being mixed.
class HighlightWriter {
  public:
    using VideoEncoder = cutting::VideoEncoder;

    HighlightWriter(const std::filesystem::path& path, const VideoFormat& format,
                    const RenderOptions& options, ProgressReporter& progress)
        : output_(path), format_(format),
          video_(cutting::open_video_encoder(options.encoding, format, path, progress)) {
        video_out_ = cutting::add_encoded_stream(output_, *video_.context());
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        audio_.emplace(kAudioRate, stereo, AV_SAMPLE_FMT_FLTP);
        audio_out_ = cutting::add_encoded_stream(output_, *audio_->context());
        output_.open();
    }

    [[nodiscard]] const VideoEncoder& video() const noexcept { return video_; }

    /// A picture of a clip (frame j of `timing`): blended with the previous clip's picture in
    /// the fade-in, held back for the next clip in the fade-out, written otherwise.
    void add_picture(const ClipTiming& timing, std::int64_t j, AVFrame& picture, bool last_clip) {
        if (j < timing.fade_in) {
            const auto weight = static_cast<int>((j + 1) * kWeightOne / (timing.fade_in + 1));
            AVFrame& previous = *held_[static_cast<std::size_t>(j)];
            blend(previous, picture, weight);
            write_picture(previous);
            return;
        }
        if (!last_clip && j >= timing.frames - timing.fade_out) {
            auto copy = make_picture(format_);
            ff::check(av_frame_copy(copy.get(), &picture), "frame copy");
            holding_.push_back(std::move(copy));
            return;
        }
        write_picture(picture);
    }

    /// After a clip: its fade-out pictures wait for the next clip.
    void end_clip() {
        held_ = std::move(holding_);
        holding_.clear();
    }

    /// Mixes a clip's sound (from output sample `first`) with fades of `fade_in` and `fade_out`
    /// samples, and writes everything before `keep_from` (the next clip's first sample).
    void add_sound(const AudioPlanes& sound, std::int64_t first, std::int64_t fade_in,
                   std::int64_t fade_out, std::int64_t keep_from) {
        const auto length = static_cast<std::int64_t>(sound[0].size());
        const std::int64_t needed = first + length - mix_start_;
        for (auto& channel : mix_) {
            if (static_cast<std::int64_t>(channel.size()) < needed) {
                channel.resize(static_cast<std::size_t>(needed), 0.0F);
            }
        }
        for (std::int64_t k = 0; k < length; ++k) {
            float weight = 1.0F;
            if (k < fade_in) {
                weight = static_cast<float>(k + 1) / static_cast<float>(fade_in + 1);
            } else if (k >= length - fade_out) {
                weight = static_cast<float>(length - k) / static_cast<float>(fade_out + 1);
            }
            const auto at = static_cast<std::size_t>(first + k - mix_start_);
            for (int c = 0; c < kAudioChannels; ++c) {
                mix_[static_cast<std::size_t>(c)][at] +=
                    weight * sound[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
            }
        }
        flush_sound(keep_from);
    }

    void finish() {
        flush_sound(mix_start_ + static_cast<std::int64_t>(mix_[0].size()));
        audio_->send_ready(true);
        cutting::drain(audio_->context(), audio_out_, output_, true);
        video_.drain(video_out_, output_, true);
        output_.finish();
    }

    [[nodiscard]] std::int64_t frames_written() const noexcept { return frames_written_; }

  private:
    void write_picture(AVFrame& picture) {
        video_.send(picture, frames_written_++);
        video_.drain(video_out_, output_, false);
    }

    /// Writes the mixed sound before `until`, a few encoder frames at a time (the encoder
    /// takes only a few before its packets must be collected).
    void flush_sound(std::int64_t until) {
        const std::int64_t count =
            std::min(until - mix_start_, static_cast<std::int64_t>(mix_[0].size()));
        if (count <= 0) {
            return;
        }
        const std::int64_t chunk = audio_->context()->frame_size;
        for (std::int64_t done = 0; done < count; done += chunk) {
            const std::int64_t size = std::min(chunk, count - done);
            const std::array<const float*, kAudioChannels> planes{mix_[0].data() + done,
                                                                  mix_[1].data() + done};
            audio_->append_planar(planes.data(), static_cast<int>(size));
            audio_->send_ready(false);
            cutting::drain(audio_->context(), audio_out_, output_, false);
        }
        for (auto& channel : mix_) {
            channel.erase(channel.begin(), channel.begin() + count);
        }
        mix_start_ += count;
    }

    cutting::Output output_;
    VideoFormat format_;
    VideoEncoder video_;
    std::optional<cutting::AudioEncoder> audio_;
    AVStream* video_out_ = nullptr;
    AVStream* audio_out_ = nullptr;
    std::vector<ff::FramePtr> held_;    ///< fade-out pictures of the previous clip
    std::vector<ff::FramePtr> holding_; ///< fade-out pictures of the current clip
    AudioPlanes mix_;
    std::int64_t mix_start_ = 0; ///< output sample of mix_[c][0]
    std::int64_t frames_written_ = 0;
};

/// Size, frame rate and colours of the first clip's video.
VideoFormat output_format(const Clip& first) {
    const auto format = ff::open_input(first.video);
    const int index = best_stream(format.get(), AVMEDIA_TYPE_VIDEO);
    if (index < 0) {
        throw MediaError(first.video.string() + " has no video stream");
    }
    const auto decoder = ff::open_decoder(format->streams[index]);
    VideoFormat result = cutting::video_format_of(*decoder, *format->streams[index]);
    result.width &= ~1; // 4:2:0 needs even sizes
    result.height &= ~1;
    if (result.frame_rate.num <= 0 || result.width <= 0 || result.height <= 0) {
        throw MediaError(first.video.string() + ": unknown frame rate or size");
    }
    return result;
}

cutting::CutResult render_clips(std::span<const Clip> clips, const std::filesystem::path& output,
                                const RenderOptions& options, ProgressReporter& progress) {
    const VideoFormat format = output_format(clips.front());
    const std::vector<ClipTiming> timing =
        plan_timing(clips, format.frame_rate, std::max(0.0, options.crossfade_s));
    HighlightWriter writer(output, format, options, progress);
    TitleCache titles(options.font);
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const Clip& clip = clips[i];
        const ClipTiming& t = timing[i];
        const bool last_clip = i + 1 == clips.size();
        progress.report(std::format("  clip {} of {}: {} {:.1f}-{:.1f} s", i + 1, clips.size(),
                                    clip.video.filename().string(), clip.start_s, clip.end_s));
        std::vector<std::pair<const TitleMask*, const Title*>> painted;
        for (const Title& title : clip.titles) {
            if (title.visible()) {
                painted.emplace_back(&titles.mask(title, format.height), &title);
            }
        }
        const std::int64_t first_sample = sample_at_frame(t.first, format.frame_rate);
        const std::int64_t end_sample = sample_at_frame(t.end(), format.frame_rate);
        AudioPlanes sound;
        for (auto& channel : sound) {
            channel.assign(static_cast<std::size_t>(end_sample - first_sample), 0.0F);
        }
        ClipReader reader(clip, format, t.frames);
        const ClipReader::PictureConsumer consume = [&](std::int64_t j, AVFrame& picture) {
            for (const auto& [mask, title] : painted) {
                paint_title({.y = picture.data[0], .u = picture.data[1], .v = picture.data[2],
                             .y_stride = picture.linesize[0], .uv_stride = picture.linesize[1],
                             .width = picture.width, .height = picture.height,
                             .full_range = format.full_range},
                            *mask, *title);
            }
            writer.add_picture(t, j, picture, last_clip);
        };
        reader.read(consume, sound);
        writer.end_clip();
        const std::int64_t fade_in =
            sample_at_frame(t.first + t.fade_in, format.frame_rate) - first_sample;
        const std::int64_t fade_out =
            end_sample - sample_at_frame(t.end() - t.fade_out, format.frame_rate);
        const std::int64_t next_first =
            last_clip ? end_sample : sample_at_frame(timing[i + 1].first, format.frame_rate);
        writer.add_sound(sound, first_sample, fade_in, fade_out, next_first);
    }
    writer.finish();
    return {writer.video().name(),
            static_cast<double>(writer.video().context()->bit_rate) / 1e6,
            static_cast<double>(writer.frames_written()) / av_q2d(format.frame_rate)};
}

} // namespace

cutting::CutResult FfmpegHighlightRenderer::render(std::span<const Clip> clips,
                                                   const std::filesystem::path& output,
                                                   const RenderOptions& options,
                                                   ProgressReporter& progress) {
    if (clips.empty()) {
        throw std::invalid_argument("no clips to render");
    }
    try {
        return render_clips(clips, output, options, progress);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(output, ignored); // never leave a half-written video
        throw;
    }
}

} // namespace ttrally::highlights
