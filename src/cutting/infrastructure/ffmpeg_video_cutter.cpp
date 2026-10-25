// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

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
constexpr int kHardwareFramePool = 16;
/// Default H.265 bit rate per pixel and frame: 4 Mbit/s for Full HD at 60 fps (chosen by the user
/// on 2026-10-25). Measured on fast rallies (hevc_vaapi, 2026-10-12): 5 Mbit/s 41.4 dB PSNR, no
/// loss visible to the eye; 8 Mbit/s 43.3 dB, 20 Mbit/s 46.5 dB. On another clip (2026-10-25)
/// 3 Mbit/s gave 39.9 dB.
constexpr double kH265BitsPerPixel = 0.032;
/// H.264 needs about twice the bit rate for the same quality (8 Mbit/s for Full HD at 60 fps).
constexpr double kH264BitsPerPixel = 0.064;
/// h264_vaapi hangs in the driver (Mesa, RX 7800 XT) at 20 Mbit/s and above or with constant
/// quality; 15 Mbit/s was measured to work.
constexpr std::int64_t kMaxH264VaapiBitrate = 15'000'000;
constexpr std::int64_t kAudioBitrate = 192'000;
constexpr double kKeyframeIntervalS = 2.0;
constexpr int kProgressSteps = 10;

// ------------------------------------------------------------------ FFmpeg resources

struct OutputCloser {
    void operator()(AVFormatContext* context) const noexcept {
        if (context == nullptr) {
            return;
        }
        if (context->pb != nullptr && (context->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
    }
};
using OutputPtr = std::unique_ptr<AVFormatContext, OutputCloser>;

struct FifoFreer {
    void operator()(AVAudioFifo* fifo) const noexcept { av_audio_fifo_free(fifo); }
};
using FifoPtr = std::unique_ptr<AVAudioFifo, FifoFreer>;

double seconds(std::int64_t timestamp, AVRational time_base) {
    return timestamp == AV_NOPTS_VALUE ? std::numeric_limits<double>::quiet_NaN()
                                       : static_cast<double>(timestamp) * av_q2d(time_base);
}

std::int64_t timestamp(double seconds, AVRational time_base) {
    return std::llround(seconds / av_q2d(time_base));
}

/// The MP4 file being written: created first, its header written once all streams exist.
class Output {
  public:
    explicit Output(const std::filesystem::path& path) : path_(path) {
        AVFormatContext* raw = nullptr;
        ff::check(avformat_alloc_output_context2(&raw, nullptr, "mp4",
                                                 ff::path_to_utf8(path).c_str()),
                  "output format");
        context_.reset(raw);
        context_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    }

    [[nodiscard]] AVStream* add_stream() {
        AVStream* stream = avformat_new_stream(context_.get(), nullptr);
        if (stream == nullptr) {
            throw MediaError("out of memory (avformat_new_stream)");
        }
        return stream;
    }

    void open() {
        std::filesystem::create_directories(path_.parent_path().empty() ? "." : path_.parent_path());
        if ((context_->oformat->flags & AVFMT_NOFILE) == 0) {
            ff::check(avio_open(&context_->pb, ff::path_to_utf8(path_).c_str(), AVIO_FLAG_WRITE),
                      "cannot write " + path_.string());
        }
        AVDictionary* options = nullptr;
        av_dict_set(&options, "movflags", "+faststart", 0); // playable while downloading
        const int result = avformat_write_header(context_.get(), &options);
        av_dict_free(&options);
        ff::check(result, "output header");
    }

    void write(AVPacket& packet) {
        ff::check(av_interleaved_write_frame(context_.get(), &packet), "writing the output");
    }

    void finish() { ff::check(av_write_trailer(context_.get()), "finishing the output"); }

    [[nodiscard]] AVFormatContext* context() const noexcept { return context_.get(); }

  private:
    std::filesystem::path path_;
    OutputPtr context_;
};

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

// ------------------------------------------------------------------ encoders

std::vector<std::string> platform_video_encoders(VideoCodec codec) {
#ifdef _WIN32
    std::vector<std::string> h265{"hevc_amf", "hevc_nvenc", "hevc_qsv", "hevc_mf"};
    std::vector<std::string> h264{"h264_amf", "h264_nvenc", "h264_qsv", "h264_mf"};
#else
    std::vector<std::string> h265{"hevc_vaapi", "hevc_nvenc", "hevc_qsv"};
    std::vector<std::string> h264{"h264_vaapi", "h264_nvenc", "h264_qsv"};
#endif
    // Software fallbacks without GPL code (no x264/x265); H.265 falls back to H.264
    h264.insert(h264.end(), {"libopenh264", "mpeg4"});
    if (codec == VideoCodec::H264) {
        return h264;
    }
    h265.insert(h265.end(), h264.begin(), h264.end());
    return h265;
}

/// Bit rate for re-encoding: as asked, else by resolution and frame rate; limited where a
/// driver is known to hang.
std::int64_t video_bitrate(const std::string& encoder, const AVStream& stream,
                           double requested_mbps, ProgressReporter& progress) {
    const AVRational rate =
        stream.avg_frame_rate.num > 0 ? stream.avg_frame_rate : stream.r_frame_rate;
    const double bits_per_pixel =
        encoder.starts_with("hevc") ? kH265BitsPerPixel : kH264BitsPerPixel;
    auto bitrate = requested_mbps > 0
                       ? static_cast<std::int64_t>(requested_mbps * 1e6)
                       : static_cast<std::int64_t>(bits_per_pixel * stream.codecpar->width *
                                                   stream.codecpar->height * av_q2d(rate));
    if (encoder == "h264_vaapi" && bitrate > kMaxH264VaapiBitrate) {
        progress.report(std::format("Note: h264_vaapi is limited to {} Mbit/s (the driver hangs "
                                    "above); use H.265 for higher quality.",
                                    kMaxH264VaapiBitrate / 1'000'000));
        bitrate = kMaxH264VaapiBitrate;
    }
    return bitrate;
}

bool is_vaapi(const std::string& name) { return name.ends_with("_vaapi"); }

/// Software pixel format the encoder takes: NV12 or YUV420P if offered, else its first one.
AVPixelFormat software_format_of(const AVCodec* codec, AVCodecContext* context) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(context, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs,
                                     &count) < 0 ||
        configs == nullptr || count == 0) {
        return AV_PIX_FMT_YUV420P;
    }
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    for (const AVPixelFormat wanted : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
        if (std::find(formats, formats + count, wanted) != formats + count) {
            return wanted;
        }
    }
    return formats[0];
}

class VideoEncoder {
  public:
    /// The encoder, or nullopt if it is missing or cannot be opened on this machine.
    static std::optional<VideoEncoder> open(const std::string& name, const AVCodecContext& decoder,
                                            const AVStream& stream, std::int64_t bitrate) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
        if (codec == nullptr) {
            return std::nullopt;
        }
        const ff::QuietLogScope quiet; // unavailable hardware is expected
        VideoEncoder encoder;
        encoder.name_ = name;
        encoder.context_.reset(avcodec_alloc_context3(codec));
        AVCodecContext* context = encoder.context_.get();
        const AVRational rate =
            stream.avg_frame_rate.num > 0 ? stream.avg_frame_rate : stream.r_frame_rate;
        context->width = decoder.width;
        context->height = decoder.height;
        context->sample_aspect_ratio = decoder.sample_aspect_ratio;
        context->framerate = rate;
        context->time_base = av_inv_q(rate);
        context->bit_rate = bitrate;
        context->rc_max_rate = 0;
        context->gop_size = static_cast<int>(std::lround(kKeyframeIntervalS * av_q2d(rate)));
        // Keep the source's colours (camera footage is often full range BT.709)
        const bool full_range = decoder.color_range == AVCOL_RANGE_JPEG ||
                                decoder.pix_fmt == AV_PIX_FMT_YUVJ420P;
        context->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        context->colorspace = decoder.colorspace;
        context->color_primaries = decoder.color_primaries;
        context->color_trc = decoder.color_trc;
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // MP4 keeps the headers in the container

        if (is_vaapi(name)) {
            if (!encoder.create_hardware_frames(AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI)) {
                return std::nullopt;
            }
        } else {
            encoder.software_format_ = software_format_of(codec, context);
            context->pix_fmt = encoder.software_format_;
        }
        if (avcodec_open2(context, codec, nullptr) < 0) {
            return std::nullopt;
        }
        return encoder;
    }

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_.get(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Converts a decoded frame to what the encoder takes and sends it with the given pts.
    void send(const AVFrame& decoded, std::int64_t pts) {
        auto converted = convert(decoded);
        if (frames_) {
            auto uploaded = ff::make_frame();
            ff::check(av_hwframe_get_buffer(frames_.get(), uploaded.get(), 0), "GPU frame");
            ff::check(av_hwframe_transfer_data(uploaded.get(), converted.get(), 0),
                      "upload to the GPU");
            ff::check(av_frame_copy_props(uploaded.get(), converted.get()), "frame properties");
            converted = std::move(uploaded);
        }
        converted->pts = pts;
        ff::check(avcodec_send_frame(context_.get(), converted.get()), "video encoding");
    }

  private:
    bool create_hardware_frames(AVHWDeviceType type, AVPixelFormat hardware_format) {
        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0) {
            return false;
        }
        device_.reset(device);
        frames_.reset(av_hwframe_ctx_alloc(device_.get()));
        if (!frames_) {
            return false;
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_->data);
        frames->format = hardware_format;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = context_->width;
        frames->height = context_->height;
        frames->initial_pool_size = kHardwareFramePool;
        if (av_hwframe_ctx_init(frames_.get()) < 0) {
            return false;
        }
        context_->pix_fmt = hardware_format;
        context_->hw_frames_ctx = av_buffer_ref(frames_.get());
        software_format_ = AV_PIX_FMT_NV12;
        return true;
    }

    ff::FramePtr convert(const AVFrame& decoded) {
        auto converted = ff::make_frame();
        converted->format = software_format_;
        converted->width = decoded.width;
        converted->height = decoded.height;
        ff::check(av_frame_get_buffer(converted.get(), 0), "frame buffer");
        scaler_.reset(sws_getCachedContext(scaler_.release(), decoded.width, decoded.height,
                                           static_cast<AVPixelFormat>(decoded.format),
                                           decoded.width, decoded.height, software_format_,
                                           SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!scaler_) {
            throw MediaError("cannot convert frames for the video encoder");
        }
        // Same range in and out: only the memory layout changes, not the values.
        const int range = context_->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        const int* coefficients = sws_getCoefficients(decoded.colorspace);
        sws_setColorspaceDetails(scaler_.get(), coefficients, range, coefficients, range, 0,
                                 1 << 16, 1 << 16);
        sws_scale(scaler_.get(), decoded.data, decoded.linesize, 0, decoded.height,
                  converted->data, converted->linesize);
        ff::check(av_frame_copy_props(converted.get(), &decoded), "frame properties");
        converted->color_range = context_->color_range;
        return converted;
    }

    std::string name_;
    ff::CodecPtr context_;
    ff::BufferPtr device_;
    ff::BufferPtr frames_; ///< GPU frame pool for hardware encoders that need uploads
    AVPixelFormat software_format_ = AV_PIX_FMT_YUV420P;
    ff::SwsPtr scaler_;
};

VideoEncoder open_video_encoder(const CutOptions& options, const AVCodecContext& decoder,
                                const AVStream& stream, ProgressReporter& progress) {
    const std::string& requested = options.encoder;
    const bool automatic = requested == "auto";
    const std::vector<std::string> candidates = automatic
                                                    ? automatic_video_encoders(options.codec)
                                                    : std::vector<std::string>{requested};
    if (automatic) {
        std::string list;
        for (const std::string& name : candidates) {
            list += (list.empty() ? "" : ", ") + name;
        }
        progress.report(std::format("Video encoders supported by this FFmpeg, tried in this "
                                    "order: {}",
                                    list.empty() ? "none" : list));
    }
    for (const std::string& name : candidates) {
        const std::int64_t bitrate = video_bitrate(name, stream, options.bitrate_mbps, progress);
        if (auto encoder = VideoEncoder::open(name, decoder, stream, bitrate)) {
            progress.report(std::format("Encoding with {} at {:.1f} Mbit/s", name,
                                        static_cast<double>(bitrate) / 1e6));
            return std::move(*encoder);
        }
        if (automatic) {
            progress.report(std::format("  {} cannot be opened on this machine", name));
        }
    }
    throw MediaError(requested == "auto" ? "no video encoder could be opened"
                                         : "the video encoder " + requested +
                                               " cannot be opened on this machine");
}

/// AAC audio, fed through a FIFO because the encoder takes fixed-size frames.
class AudioEncoder {
  public:
    explicit AudioEncoder(const AVCodecContext& decoder) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (codec == nullptr) {
            throw MediaError("FFmpeg lacks an AAC encoder");
        }
        context_.reset(avcodec_alloc_context3(codec));
        context_->sample_rate = decoder.sample_rate;
        ff::check(av_channel_layout_copy(&context_->ch_layout, &decoder.ch_layout),
                  "channel layout");
        context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        context_->bit_rate = kAudioBitrate;
        context_->time_base = {1, decoder.sample_rate};
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ff::check(avcodec_open2(context_.get(), codec, nullptr), "cannot open the AAC encoder");
        if (decoder.sample_fmt != AV_SAMPLE_FMT_FLTP) {
            SwrContext* resampler = nullptr;
            ff::check(swr_alloc_set_opts2(&resampler, &context_->ch_layout, AV_SAMPLE_FMT_FLTP,
                                          decoder.sample_rate, &decoder.ch_layout,
                                          decoder.sample_fmt, decoder.sample_rate, 0, nullptr),
                      "audio conversion");
            resampler_.reset(resampler);
            ff::check(swr_init(resampler_.get()), "audio conversion");
        }
        fifo_.reset(av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, context_->ch_layout.nb_channels,
                                        context_->frame_size));
        if (!fifo_) {
            throw MediaError("out of memory (audio FIFO)");
        }
    }

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_.get(); }

    /// Appends samples [first, first + count) of a decoded frame.
    void append(const AVFrame& frame, int first, int count) {
        if (count <= 0) {
            return;
        }
        ff::FramePtr planar;
        const AVFrame* source = &frame;
        if (resampler_) {
            planar = ff::make_frame();
            planar->format = AV_SAMPLE_FMT_FLTP;
            planar->sample_rate = frame.sample_rate;
            ff::check(av_channel_layout_copy(&planar->ch_layout, &context_->ch_layout),
                      "channel layout");
            ff::check(swr_convert_frame(resampler_.get(), planar.get(), &frame),
                      "audio conversion");
            source = planar.get();
        }
        std::vector<void*> planes(static_cast<std::size_t>(context_->ch_layout.nb_channels));
        for (std::size_t c = 0; c < planes.size(); ++c) {
            planes[c] = source->extended_data[c] + static_cast<std::ptrdiff_t>(first) *
                                                       static_cast<std::ptrdiff_t>(sizeof(float));
        }
        ff::check(av_audio_fifo_write(fifo_.get(), planes.data(), count), "audio FIFO");
    }

    /// Sends complete encoder frames from the FIFO (and the rest at the end).
    void send_ready(bool final) {
        const int size = context_->frame_size;
        while (av_audio_fifo_size(fifo_.get()) >= size ||
               (final && av_audio_fifo_size(fifo_.get()) > 0)) {
            const int count = std::min(size, av_audio_fifo_size(fifo_.get()));
            auto frame = ff::make_frame();
            frame->nb_samples = count;
            frame->format = AV_SAMPLE_FMT_FLTP;
            frame->sample_rate = context_->sample_rate;
            ff::check(av_channel_layout_copy(&frame->ch_layout, &context_->ch_layout),
                      "channel layout");
            ff::check(av_frame_get_buffer(frame.get(), 0), "audio frame");
            ff::check(av_audio_fifo_read(fifo_.get(), reinterpret_cast<void**>(frame->data), count),
                      "audio FIFO");
            frame->pts = samples_sent_;
            samples_sent_ += count;
            ff::check(avcodec_send_frame(context_.get(), frame.get()), "audio encoding");
        }
    }

  private:
    ff::CodecPtr context_;
    ff::SwrPtr resampler_;
    FifoPtr fifo_;
    std::int64_t samples_sent_ = 0;
};

/// Writes all packets an encoder has ready; with flush, also the delayed ones.
void drain(AVCodecContext* encoder, AVStream* stream, Output& output, bool flush) {
    if (flush) {
        ff::check(avcodec_send_frame(encoder, nullptr), "encoder flush");
    }
    auto packet = ff::make_packet();
    for (;;) {
        const int result = avcodec_receive_packet(encoder, packet.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        ff::check(result, "encoding");
        av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
        packet->stream_index = stream->index;
        output.write(*packet);
    }
}

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
        video_encoder_.emplace(open_video_encoder(options, *video_decoder_, *video_stream_,
                                                  progress));
        video_out_ = output_.add_stream();
        ff::check(avcodec_parameters_from_context(video_out_->codecpar, video_encoder_->context()),
                  "video stream");
        // H.265 keeps the default tag hev1 (parameter sets in the stream). With hvc1, which Apple
        // players prefer, players use the parameter sets of the file header, and those written by
        // the VAAPI encoder do not match its stream (measured: hundreds of decode errors).
        video_out_->time_base = video_encoder_->context()->time_base;
        if (audio_index_ >= 0) {
            audio_stream_ = format_->streams[audio_index_];
            audio_decoder_ = ff::open_decoder(audio_stream_);
            audio_encoder_.emplace(*audio_decoder_);
            audio_out_ = output_.add_stream();
            ff::check(
                avcodec_parameters_from_context(audio_out_->codecpar, audio_encoder_->context()),
                "audio stream");
            audio_out_->time_base = audio_encoder_->context()->time_base;
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
        drain(video_encoder_->context(), video_out_, output_, true);
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
        drain(video_encoder_->context(), video_out_, output_, false);
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

std::vector<std::string> automatic_video_encoders(VideoCodec codec) {
    std::vector<std::string> available;
    for (const std::string& name : platform_video_encoders(codec)) {
        if (avcodec_find_encoder_by_name(name.c_str()) != nullptr) {
            available.push_back(name);
        }
    }
    return available;
}

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
