// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/ffmpeg_highlight_renderer.hpp"

#include "cutting/infrastructure/csv_white_balance_store.hpp"
#include "cutting/infrastructure/ffmpeg_encoding.hpp"
#include "highlights/infrastructure/svg_shape.hpp"
#include "highlights/infrastructure/title_composer.hpp"
#include "highlights/infrastructure/title_painter.hpp"
#include "media/application/media_error.hpp"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <memory>
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
                              1, std::llround(clip.shown_s() * av_q2d(rate))),
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
    /// A font that does not exist (e.g. uninstalled since it was chosen) is replaced by the
    /// default one, with a note.
    TitleCache(std::filesystem::path font, std::filesystem::path shapes, bool full_range,
               ProgressReporter& progress)
        : font_file_(std::move(font)), shapes_(std::move(shapes)), full_range_(full_range),
          progress_(progress) {}

    const PaintableTitle& title(const Title& title, int picture_height) {
        const int line_height =
            std::max(1, static_cast<int>(std::lround(title.size * picture_height)));
        const auto key = std::make_pair(look_key(title), line_height);
        if (const auto found = titles_.find(key); found != titles_.end()) {
            return found->second;
        }
        if (!font_) {
            std::error_code error;
            if (!font_file_.empty() && !std::filesystem::is_regular_file(font_file_, error)) {
                progress_.report(std::format("Note: the font {} does not exist, using the default "
                                             "font",
                                             font_file_.string()));
                font_file_.clear();
            }
            if (font_file_.empty()) {
                font_file_ = default_title_font();
            }
            if (font_file_.empty()) {
                throw std::runtime_error(
                    "no font found for the titles: give a TrueType font file with --font");
            }
            font_ = std::make_unique<TitleFont>(font_file_);
        }
        const TitleMask letters =
            font_->render(title.text, line_height, align_for(title.position));
        const TitleImage image = compose_title(letters, title, line_height, svg_of(title));
        return titles_.emplace(key, paintable(image, full_range_)).first->second;
    }

  private:
    /// The SVG drawing of the title's backdrop; nullptr (with a note) if it cannot be read.
    const SvgShape* svg_of(const Title& title) {
        const Backdrop& backdrop = title.decoration.backdrop;
        if (backdrop.shape != BackdropShape::Svg || !backdrop.visible()) {
            return nullptr;
        }
        try {
            return &svgs_.load(shapes_ / backdrop.svg);
        } catch (const std::exception& failure) {
            progress_.report(std::format("Note: {}; the title is drawn without its backdrop",
                                         failure.what()));
            return nullptr;
        }
    }

    std::filesystem::path font_file_;
    std::filesystem::path shapes_;
    bool full_range_;
    ProgressReporter& progress_;
    std::unique_ptr<TitleFont> font_;
    SvgShapeCache svgs_;
    std::map<std::pair<std::string, int>, PaintableTitle> titles_;
};

/// A section of a decoded picture: its planes from the section's corner, and its size.
struct Section {
    std::array<const std::uint8_t*, AV_NUM_DATA_POINTERS> data{};
    int width = 0;
    int height = 0;
};

/// The slow motion's section of a picture at full size (corners on whole chroma samples).
Section section_of(const AVFrame& frame, const SlowPlayback& slow) {
    const AVPixFmtDescriptor* format =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame.format));
    const int align_x = 1 << format->log2_chroma_w;
    const int align_y = 1 << format->log2_chroma_h;
    const auto aligned = [](double value, int step) {
        return static_cast<int>(std::lround(value / step)) * step;
    };
    Section section;
    section.width = std::max(align_x, aligned(slow.size * frame.width, align_x));
    section.height = std::max(align_y, aligned(slow.size * frame.height, align_y));
    const int x = std::clamp(aligned(slow.left * frame.width, align_x), 0,
                             (frame.width - section.width) / align_x * align_x);
    const int y = std::clamp(aligned(slow.top * frame.height, align_y), 0,
                             (frame.height - section.height) / align_y * align_y);
    for (int plane = 0; plane < AV_NUM_DATA_POINTERS && frame.data[plane] != nullptr; ++plane) {
        // Bytes per pixel and subsampling of the plane, from its first component
        int step = 1;
        bool chroma = false;
        for (int c = 0; c < format->nb_components; ++c) {
            if (format->comp[c].plane == plane) {
                step = format->comp[c].step;
                chroma = (c == 1 || c == 2) && (format->flags & AV_PIX_FMT_FLAG_RGB) == 0;
                break;
            }
        }
        const int px = chroma ? x >> format->log2_chroma_w : x;
        const int py = chroma ? y >> format->log2_chroma_h : y;
        section.data[static_cast<std::size_t>(plane)] =
            frame.data[plane] + static_cast<std::ptrdiff_t>(py) * frame.linesize[plane] +
            static_cast<std::ptrdiff_t>(px) * step;
    }
    return section;
}

/// The sound stretched `factor` times longer at the same pitch (FFmpeg's atempo), written into
/// `into` from its start (its size is kept).
void stretch_sound(const AudioPlanes& sound, int factor, AudioPlanes& into) {
    AVFilterGraph* raw = avfilter_graph_alloc();
    if (raw == nullptr) {
        throw MediaError("cannot stretch the sound");
    }
    const std::unique_ptr<AVFilterGraph, void (*)(AVFilterGraph*)> graph(
        raw, [](AVFilterGraph* g) { avfilter_graph_free(&g); });
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    const std::string arguments = std::format(
        "time_base=1/{0}:sample_rate={0}:sample_fmt=fltp:channel_layout=stereo", kAudioRate);
    ff::check(avfilter_graph_create_filter(&source, avfilter_get_by_name("abuffer"), "in",
                                           arguments.c_str(), nullptr, graph.get()),
              "sound filter");
    ff::check(avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out",
                                           nullptr, nullptr, graph.get()),
              "sound filter");
    // atempo slows down to half at most per instance: 2 = 0.5; 3 = 0.5 * 0.667; 4 = 0.5 * 0.5
    AVFilterContext* previous = source;
    double rest = 1.0 / factor;
    for (int i = 0; rest < 1.0 - 1e-9; ++i) {
        const double tempo = std::max(0.5, rest);
        AVFilterContext* tempo_filter = nullptr;
        ff::check(avfilter_graph_create_filter(&tempo_filter, avfilter_get_by_name("atempo"),
                                               std::format("tempo{}", i).c_str(),
                                               std::format("tempo={:.6f}", tempo).c_str(),
                                               nullptr, graph.get()),
                  "sound filter");
        ff::check(avfilter_link(previous, 0, tempo_filter, 0), "sound filter");
        previous = tempo_filter;
        rest /= tempo;
    }
    // Back to planar float stereo, as the mix takes it
    AVFilterContext* planar = nullptr;
    ff::check(avfilter_graph_create_filter(&planar, avfilter_get_by_name("aformat"), "planar",
                                           "sample_fmts=fltp:channel_layouts=stereo", nullptr,
                                           graph.get()),
              "sound filter");
    ff::check(avfilter_link(previous, 0, planar, 0), "sound filter");
    ff::check(avfilter_link(planar, 0, sink, 0), "sound filter");
    ff::check(avfilter_graph_config(graph.get(), nullptr), "sound filter");

    std::size_t written = 0;
    const auto collect = [&] {
        auto out = ff::make_frame();
        while (av_buffersink_get_frame(sink, out.get()) >= 0) {
            for (int k = 0; k < out->nb_samples && written < into[0].size(); ++k, ++written) {
                for (int c = 0; c < kAudioChannels; ++c) {
                    into[static_cast<std::size_t>(c)][written] =
                        reinterpret_cast<const float*>(out->extended_data[c])[k];
                }
            }
            av_frame_unref(out.get());
        }
    };
    constexpr int kChunk = 1024;
    const std::size_t total = sound[0].size();
    for (std::size_t at = 0; at < total; at += kChunk) {
        const int count = static_cast<int>(std::min<std::size_t>(kChunk, total - at));
        auto frame = ff::make_frame();
        frame->format = AV_SAMPLE_FMT_FLTP;
        frame->sample_rate = kAudioRate;
        frame->nb_samples = count;
        frame->pts = static_cast<std::int64_t>(at);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        ff::check(av_channel_layout_copy(&frame->ch_layout, &stereo), "channel layout");
        ff::check(av_frame_get_buffer(frame.get(), 0), "sound buffer");
        for (int c = 0; c < kAudioChannels; ++c) {
            const auto& channel = sound[static_cast<std::size_t>(c)];
            std::copy_n(channel.begin() + static_cast<std::ptrdiff_t>(at), count,
                        reinterpret_cast<float*>(frame->extended_data[c]));
        }
        ff::check(av_buffersrc_add_frame(source, frame.get()), "sound filter");
        collect();
    }
    ff::check(av_buffersrc_add_frame(source, nullptr), "sound filter");
    collect();
}

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
        if (clip_.slow) {
            // Silence, or the sound of the clip's time stretched to the slower pace afterwards
            audio_done_ = !clip_.slow->sound;
            const auto samples =
                static_cast<std::size_t>(std::llround((clip_.end_s - clip_.start_s) * kAudioRate));
            for (auto& channel : source_sound_) {
                channel.assign(samples, 0.0F);
            }
        }
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
        if (clip_.slow && clip_.slow->sound) {
            stretch_sound(source_sound_, clip_.slow->factor, audio);
        }
    }

  private:
    [[nodiscard]] bool audio_done() const noexcept { return !audio_decoder_ || audio_done_; }

    /// Time on the video of the clip's output frame `n` (a slow motion runs `factor` times
    /// slower).
    [[nodiscard]] double source_time(std::int64_t n) const {
        const double factor = clip_.slow ? clip_.slow->factor : 1.0;
        return clip_.start_s + frame_time(n, output_.frame_rate) / factor;
    }

    void seek() {
        const auto target = static_cast<std::int64_t>(
            std::floor(clip_.start_s / av_q2d(video_stream_->time_base)));
        // A failed seek (e.g. a file without index) reads from the start, which is only slower
        ff::seek_before(format_.get(), *video_stream_, target);
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
        while (next_ < frames_ && source_time(next_) < t - kTimeEpsilon) {
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
        if (clip_.slow && clip_.slow->size < 1.0) {
            const Section section = section_of(decoded, *clip_.slow);
            scaler_.reset(sws_getCachedContext(
                scaler_.release(), section.width, section.height,
                static_cast<AVPixelFormat>(decoded.format), output_.width, output_.height,
                AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr));
            if (!scaler_) {
                throw MediaError("cannot scale the section of " + clip_.video.string());
            }
            sws_setColorspaceDetails(scaler_.get(), sws_getCoefficients(decoded.colorspace),
                                     source_full ? 1 : 0, sws_getCoefficients(output_.colorspace),
                                     output_.full_range ? 1 : 0, 0, 1 << 16, 1 << 16);
            sws_scale(scaler_.get(), section.data.data(), decoded.linesize, 0, section.height,
                      picture->data, picture->linesize);
            return picture;
        }
        sws_scale(scaler_.get(), decoded.data, decoded.linesize, 0, decoded.height,
                  picture->data, picture->linesize);
        return picture;
    }

    void take_sound(const AVFrame& frame) {
        const double t = ff::to_seconds(frame.best_effort_timestamp, audio_stream_->time_base);
        if (std::isnan(t)) {
            return;
        }
        const double clip_end = source_time(frames_);
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
        AudioPlanes& target = clip_.slow ? source_sound_ : *audio_;
        const auto size = static_cast<std::int64_t>(target[0].size());
        for (int c = 0; c < kAudioChannels; ++c) {
            const auto* samples = reinterpret_cast<const float*>(converted->extended_data[c]);
            std::vector<float>& channel = target[static_cast<std::size_t>(c)];
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
    AudioPlanes source_sound_; ///< a slow motion's sound before it is stretched
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
    std::map<std::filesystem::path, std::vector<cutting::ColorSpan>> spans;
    const auto color_of = [&](const std::filesystem::path& video)
        -> const std::vector<cutting::ColorSpan>& {
        auto found = spans.find(video);
        if (found == spans.end()) {
            found = spans
                        .emplace(video,
                                 cutting::color_spans(video, options.encoding.white_balance))
                        .first;
        }
        return found->second;
    };
    const VideoFormat format =
        cutting::limited_to(output_format(clips.front()), options.encoding.max_height);
    constexpr int kStandardDefinition = 576; ///< up to this height BT.601 is assumed
    const bool high_definition = format.height > kStandardDefinition;
    const cutting::YuvFormat yuv{
        .bt709 = format.colorspace == AVCOL_SPC_BT709 ||
                 (format.colorspace == AVCOL_SPC_UNSPECIFIED && high_definition),
        .full_range = format.full_range};
    const std::vector<ClipTiming> timing =
        plan_timing(clips, format.frame_rate, std::max(0.0, options.crossfade_s));
    HighlightWriter writer(output, format, options, progress);
    TitleCache titles(options.font, options.shapes, format.full_range, progress);
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const Clip& clip = clips[i];
        const ClipTiming& t = timing[i];
        const bool last_clip = i + 1 == clips.size();
        progress.report(std::format("  clip {} of {}: {} {:.1f}-{:.1f} s", i + 1, clips.size(),
                                    clip.video.filename().string(), clip.start_s, clip.end_s));
        std::vector<std::pair<const PaintableTitle*, TitlePosition>> painted;
        for (const Title& title : clip.titles) {
            if (title.visible()) {
                painted.emplace_back(&titles.title(title, format.height), title.position);
            }
        }
        const std::int64_t first_sample = sample_at_frame(t.first, format.frame_rate);
        const std::int64_t end_sample = sample_at_frame(t.end(), format.frame_rate);
        AudioPlanes sound;
        for (auto& channel : sound) {
            channel.assign(static_cast<std::size_t>(end_sample - first_sample), 0.0F);
        }
        ClipReader reader(clip, format, t.frames);
        const std::vector<cutting::ColorSpan>& color = color_of(clip.video);
        const ClipReader::PictureConsumer consume = [&](std::int64_t j, AVFrame& picture) {
            // The white balance first: the titles keep their colours
            const double source_s =
                clip.start_s + static_cast<double>(j) / av_q2d(format.frame_rate) /
                                   (clip.slow ? clip.slow->factor : 1);
            cutting::apply_white_balance(picture, cutting::gains_at(color, source_s), yuv);
            for (const auto& [title, position] : painted) {
                paint_title({.y = picture.data[0], .u = picture.data[1], .v = picture.data[2],
                             .y_stride = picture.linesize[0], .uv_stride = picture.linesize[1],
                             .width = picture.width, .height = picture.height,
                             .full_range = format.full_range},
                            *title, position);
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
