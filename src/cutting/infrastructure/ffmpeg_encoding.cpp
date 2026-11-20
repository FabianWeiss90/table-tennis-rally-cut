// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/ffmpeg_encoding.hpp"

#include "cutting/infrastructure/encoder_probe.hpp"
#include "cutting/infrastructure/parameter_sets.hpp"
#include "cutting/infrastructure/ffmpeg_video_cutter.hpp"
#include "media/application/media_error.hpp"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace ttrally::cutting {

namespace ff = media::ff;
using media::MediaError;

namespace {

constexpr int kHardwareFramePool = 16;
/// Default H.265 bit rate per pixel and frame: 4 Mbit/s for Full HD at 60 fps (chosen by the user
/// on 2026-10-25). Measured on fast rallies (hevc_vaapi, 2026-10-12): 5 Mbit/s 41.4 dB PSNR, no
/// loss visible to the eye; 8 Mbit/s 43.3 dB, 20 Mbit/s 46.5 dB. On another clip (2026-10-25)
/// 3 Mbit/s gave 39.9 dB.
constexpr double kH265BitsPerPixel = 0.032;
/// H.264 needs about twice the bit rate for the same quality (8 Mbit/s for Full HD at 60 fps,
/// after rounding to whole Mbit/s).
constexpr double kH264BitsPerPixel = 0.064;
constexpr std::int64_t kAudioBitrate = 192'000;
constexpr double kKeyframeIntervalS = 2.0;
/// A single encoder call takes milliseconds; one that has not returned after this long hangs
/// (seen with h264_vaapi on Mesa 26.2 before its settings were fixed; kept as a guard).
constexpr std::chrono::seconds kEncoderHangTimeout{20};
constexpr std::chrono::milliseconds kWatchdogPoll{500};
/// Bit rate suggested after a hang: the H.264 default, else the next step above it.
constexpr double kSuggestedBitrateMbps = 8.0;
constexpr double kSuggestedBitrateAlternativeMbps = 10.0;

std::vector<std::string> platform_video_encoders(VideoCodec codec) {
    // Software encoders without GPL code (no x264/x265); H.265 falls back to H.264
#ifdef _WIN32
    std::vector<std::string> h265{"hevc_amf", "hevc_nvenc", "hevc_qsv", "hevc_mf"};
    std::vector<std::string> h264{"h264_amf", "h264_nvenc", "h264_qsv",
                                  "h264_mf",  "libopenh264", "mpeg4"};
#else
    std::vector<std::string> h265{"hevc_vaapi", "hevc_nvenc", "hevc_qsv"};
    std::vector<std::string> h264{"h264_vaapi", "h264_nvenc", "h264_qsv", "libopenh264",
                                  "mpeg4"};
#endif
    if (codec == VideoCodec::H264) {
        return h264;
    }
    h265.insert(h265.end(), h264.begin(), h264.end());
    return h265;
}

/// Bit rate for re-encoding: as asked, else by resolution and frame rate, rounded to whole
/// Mbit/s.
std::int64_t video_bitrate(const std::string& encoder, const VideoFormat& format,
                           double requested_mbps) {
    const double bits_per_pixel =
        encoder.starts_with("hevc") ? kH265BitsPerPixel : kH264BitsPerPixel;
    constexpr double kMbit = 1e6;
    const double by_format = bits_per_pixel * format.width * format.height *
                             av_q2d(format.frame_rate) / kMbit;
    return static_cast<std::int64_t>(
        (requested_mbps > 0 ? requested_mbps : std::max(1.0, std::round(by_format))) * kMbit);
}

bool is_vaapi(const std::string& name) { return name.ends_with("_vaapi"); }

/// What to tell the user when the encoder hangs.
std::string hang_advice(const std::string& encoder, double mbps, std::chrono::seconds timeout) {
    const double other = std::lround(mbps) == std::lround(kSuggestedBitrateMbps)
                             ? kSuggestedBitrateAlternativeMbps
                             : kSuggestedBitrateMbps;
    return std::format(
        "Error: the video encoder {} has not answered for {} s and was stopped; the graphics "
        "driver hangs at this bit rate ({:.1f} Mbit/s). The unfinished video was removed. Try "
        "again with another bit rate, e.g. {:g} Mbit/s, or with another encoder.",
        encoder, timeout.count(), mbps, other);
}

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

} // namespace

// ------------------------------------------------------------------ watchdog

/// Ends the process when a call into the encoder does not return in time.
class EncoderWatchdog {
  public:
    EncoderWatchdog(std::string advice, std::filesystem::path output)
        : advice_(std::move(advice)), output_(std::move(output)),
          thread_([this] { watch(); }) {}
    EncoderWatchdog(const EncoderWatchdog&) = delete;
    EncoderWatchdog& operator=(const EncoderWatchdog&) = delete;
    EncoderWatchdog(EncoderWatchdog&&) = delete;
    EncoderWatchdog& operator=(EncoderWatchdog&&) = delete;

    ~EncoderWatchdog() {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    /// Marks a call into the encoder for as long as it lives.
    class Call {
      public:
        explicit Call(EncoderWatchdog* watchdog) : watchdog_(watchdog) {
            if (watchdog_ != nullptr) {
                watchdog_->set_busy(true);
            }
        }
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        Call(Call&&) = delete;
        Call& operator=(Call&&) = delete;
        ~Call() {
            if (watchdog_ != nullptr) {
                watchdog_->set_busy(false);
            }
        }

      private:
        EncoderWatchdog* watchdog_;
    };

  private:
    void set_busy(bool busy) {
        const std::lock_guard lock(mutex_);
        busy_since_ = busy ? std::optional(std::chrono::steady_clock::now()) : std::nullopt;
    }

    void watch() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            wake_.wait_for(lock, kWatchdogPoll);
            if (busy_since_ &&
                std::chrono::steady_clock::now() - *busy_since_ > kEncoderHangTimeout) {
                give_up();
            }
        }
    }

    /// The hanging call never returns: report, clean up and end the process right here.
    [[noreturn]] void give_up() const {
        std::fflush(stdout);
        std::fputs(advice_.c_str(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
        std::error_code ignored;
        std::filesystem::remove(output_, ignored);
        std::_Exit(kEncoderHangExitCode);
    }

    std::string advice_;
    std::filesystem::path output_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<std::chrono::steady_clock::time_point> busy_since_;
    bool stopping_ = false;
    std::thread thread_; ///< last: starts once everything it uses exists
};

// ------------------------------------------------------------------ output

void OutputCloser::operator()(AVFormatContext* context) const noexcept {
    if (context == nullptr) {
        return;
    }
    if (context->pb != nullptr && (context->oformat->flags & AVFMT_NOFILE) == 0) {
        avio_closep(&context->pb);
    }
    avformat_free_context(context);
}

Output::Output(const std::filesystem::path& path) : path_(path) {
    AVFormatContext* raw = nullptr;
    ff::check(avformat_alloc_output_context2(&raw, nullptr, "mp4", ff::path_to_utf8(path).c_str()),
              "output format");
    context_.reset(raw);
    context_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
}

AVStream* Output::add_stream() {
    AVStream* stream = avformat_new_stream(context_.get(), nullptr);
    if (stream == nullptr) {
        throw MediaError("out of memory (avformat_new_stream)");
    }
    return stream;
}

void Output::take_parameter_sets_from_stream(AVStream* stream) {
    AVCodecParameters& parameters = *stream->codecpar;
    av_freep(&parameters.extradata);
    parameters.extradata_size = 0;
    if (parameters.codec_id == AV_CODEC_ID_HEVC) {
        parameters.codec_tag = MKTAG('h', 'v', 'c', '1');
    }
    waits_for_ = stream;
}

void Output::open() {
    std::filesystem::create_directories(path_.parent_path().empty() ? "." : path_.parent_path());
    if ((context_->oformat->flags & AVFMT_NOFILE) == 0) {
        ff::check(avio_open(&context_->pb, ff::path_to_utf8(path_).c_str(), AVIO_FLAG_WRITE),
                  "cannot write " + path_.string());
    }
    if (waits_for_ == nullptr) {
        write_header();
    }
}

void Output::write_header() {
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0); // playable while downloading
    const int result = avformat_write_header(context_.get(), &options);
    av_dict_free(&options);
    ff::check(result, "output header");
    header_written_ = true;
}

void Output::take_parameter_sets(const AVPacket& packet) {
    const NalCodec codec =
        waits_for_->codecpar->codec_id == AV_CODEC_ID_HEVC ? NalCodec::H265 : NalCodec::H264;
    const std::span<const std::uint8_t> data(packet.data, static_cast<std::size_t>(packet.size));
    const Bytes sets = to_annexb(parameter_sets(annexb_units(data), codec));
    if (sets.empty()) {
        throw MediaError("the first video packet has no parameter sets; cannot write " +
                         path_.string());
    }
    AVCodecParameters& parameters = *waits_for_->codecpar;
    parameters.extradata =
        static_cast<std::uint8_t*>(av_mallocz(sets.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters.extradata == nullptr) {
        throw MediaError("out of memory (parameter sets)");
    }
    std::ranges::copy(sets, parameters.extradata);
    parameters.extradata_size = static_cast<int>(sets.size());
}

void Output::write(AVPacket& packet) {
    if (!header_written_) {
        if (packet.stream_index != waits_for_->index) {
            auto held = ff::make_packet();
            ff::check(av_packet_ref(held.get(), &packet), "holding a packet");
            held_.push_back(std::move(held));
            return;
        }
        take_parameter_sets(packet);
        write_header();
        for (const ff::PacketPtr& held : held_) {
            ff::check(av_interleaved_write_frame(context_.get(), held.get()),
                      "writing the output");
        }
        held_.clear();
    }
    ff::check(av_interleaved_write_frame(context_.get(), &packet), "writing the output");
}

void Output::finish() {
    if (!header_written_) {
        throw MediaError("no video was written to " + path_.string());
    }
    ff::check(av_write_trailer(context_.get()), "finishing the output");
}

// ------------------------------------------------------------------ video

VideoFormat video_format_of(const AVCodecContext& decoder, const AVStream& stream) {
    return {.width = decoder.width,
            .height = decoder.height,
            .sample_aspect_ratio = decoder.sample_aspect_ratio,
            .frame_rate = stream.avg_frame_rate.num > 0 ? stream.avg_frame_rate
                                                        : stream.r_frame_rate,
            .full_range = decoder.color_range == AVCOL_RANGE_JPEG ||
                          decoder.pix_fmt == AV_PIX_FMT_YUVJ420P,
            .colorspace = decoder.colorspace,
            .color_primaries = decoder.color_primaries,
            .color_trc = decoder.color_trc};
}

std::optional<VideoEncoder> VideoEncoder::open(const std::string& name, const VideoFormat& format,
                                               std::int64_t bitrate) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (codec == nullptr) {
        return std::nullopt;
    }
    const ff::QuietLogScope quiet; // unavailable hardware is expected
    VideoEncoder encoder;
    encoder.name_ = name;
    encoder.context_.reset(avcodec_alloc_context3(codec));
    AVCodecContext* context = encoder.context_.get();
    context->width = format.width;
    context->height = format.height;
    context->sample_aspect_ratio = format.sample_aspect_ratio;
    context->framerate = format.frame_rate;
    context->time_base = av_inv_q(format.frame_rate);
    context->bit_rate = bitrate;
    context->rc_max_rate = 0;
    context->gop_size =
        static_cast<int>(std::lround(kKeyframeIntervalS * av_q2d(format.frame_rate)));
    // Keep the source's colours (camera footage is often full range BT.709)
    context->color_range = format.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    context->colorspace = format.colorspace;
    context->color_primaries = format.color_primaries;
    context->color_trc = format.color_trc;
    if (!writes_parameter_sets_in_stream(*codec)) {
        context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // MP4 keeps the headers in the container
    }

    if (is_vaapi(name)) {
        if (!encoder.create_hardware_frames(AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI)) {
            return std::nullopt;
        }
    } else {
        encoder.software_format_ = software_format_of(codec, context);
        context->pix_fmt = encoder.software_format_;
    }
    AVDictionary* options = nullptr;
    for (const auto& [option, value] : encoder_settings(name)) {
        av_dict_set(&options, option.c_str(), value.c_str(), 0);
    }
    const int opened = avcodec_open2(context, codec, &options);
    av_dict_free(&options);
    if (opened < 0) {
        return std::nullopt;
    }
    return encoder;
}

void VideoEncoder::send(const AVFrame& frame, std::int64_t pts) {
    auto converted = convert(frame);
    if (frames_) {
        auto uploaded = ff::make_frame();
        ff::check(av_hwframe_get_buffer(frames_.get(), uploaded.get(), 0), "GPU frame");
        ff::check(av_hwframe_transfer_data(uploaded.get(), converted.get(), 0),
                  "upload to the GPU");
        ff::check(av_frame_copy_props(uploaded.get(), converted.get()), "frame properties");
        converted = std::move(uploaded);
    }
    converted->pts = pts;
    const EncoderWatchdog::Call call(watchdog_.get());
    ff::check(avcodec_send_frame(context_.get(), converted.get()), "video encoding");
}

void VideoEncoder::drain(AVStream* stream, Output& output, bool flush) {
    const EncoderWatchdog::Call call(watchdog_.get());
    cutting::drain(context_.get(), stream, output, flush);
}

void VideoEncoder::exit_on_hang(std::string advice, std::filesystem::path output) {
    watchdog_ = std::make_shared<EncoderWatchdog>(std::move(advice), std::move(output));
}

bool VideoEncoder::create_hardware_frames(AVHWDeviceType type, AVPixelFormat hardware_format) {
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

ff::FramePtr VideoEncoder::convert(const AVFrame& decoded) {
    auto converted = ff::make_frame();
    converted->format = software_format_;
    converted->width = decoded.width;
    converted->height = decoded.height;
    ff::check(av_frame_get_buffer(converted.get(), 0), "frame buffer");
    scaler_.reset(sws_getCachedContext(scaler_.release(), decoded.width, decoded.height,
                                       static_cast<AVPixelFormat>(decoded.format), decoded.width,
                                       decoded.height, software_format_, SWS_BILINEAR, nullptr,
                                       nullptr, nullptr));
    if (!scaler_) {
        throw MediaError("cannot convert frames for the video encoder");
    }
    // Same range in and out: only the memory layout changes, not the values.
    const int range = context_->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    const int* coefficients = sws_getCoefficients(decoded.colorspace);
    sws_setColorspaceDetails(scaler_.get(), coefficients, range, coefficients, range, 0, 1 << 16,
                             1 << 16);
    sws_scale(scaler_.get(), decoded.data, decoded.linesize, 0, decoded.height, converted->data,
              converted->linesize);
    ff::check(av_frame_copy_props(converted.get(), &decoded), "frame properties");
    converted->color_range = context_->color_range;
    return converted;
}

VideoEncoder open_video_encoder(const CutOptions& options, const VideoFormat& format,
                                const std::filesystem::path& output,
                                ProgressReporter& progress) {
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
    EncoderFailures failures(default_encoder_failures_file());
    for (const std::string& name : candidates) {
        std::int64_t bitrate = video_bitrate(name, format, options.bitrate_mbps);
        if (is_vaapi(name)) {
            // The Mesa driver fails silently at some bit rates: try first, take a neighbour
            const std::int64_t max = std::numeric_limits<std::int64_t>::max();
            const std::optional<std::int64_t> working =
                working_bitrate(name, format, bitrate, max, failures, progress);
            if (!working) {
                if (automatic) {
                    progress.report(std::format("  {} works at no bit rate near {:.1f} Mbit/s",
                                                name, static_cast<double>(bitrate) / 1e6));
                }
                continue;
            }
            bitrate = *working;
        }
        if (auto encoder = VideoEncoder::open(name, format, bitrate)) {
            const double mbps = static_cast<double>(bitrate) / 1e6;
            progress.report(std::format("Encoding with {} at {:.1f} Mbit/s", name, mbps));
            encoder->exit_on_hang(hang_advice(name, mbps, kEncoderHangTimeout), output);
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

std::vector<std::string> automatic_video_encoders(VideoCodec codec) {
    std::vector<std::string> available;
    for (const std::string& name : platform_video_encoders(codec)) {
        if (avcodec_find_encoder_by_name(name.c_str()) != nullptr) {
            available.push_back(name);
        }
    }
    return available;
}

// ------------------------------------------------------------------ audio

AudioEncoder::AudioEncoder(int sample_rate, const AVChannelLayout& layout,
                           AVSampleFormat input_format) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        throw MediaError("FFmpeg lacks an AAC encoder");
    }
    context_.reset(avcodec_alloc_context3(codec));
    context_->sample_rate = sample_rate;
    ff::check(av_channel_layout_copy(&context_->ch_layout, &layout), "channel layout");
    context_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    context_->bit_rate = kAudioBitrate;
    context_->time_base = {1, sample_rate};
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ff::check(avcodec_open2(context_.get(), codec, nullptr), "cannot open the AAC encoder");
    if (input_format != AV_SAMPLE_FMT_FLTP) {
        SwrContext* resampler = nullptr;
        ff::check(swr_alloc_set_opts2(&resampler, &context_->ch_layout, AV_SAMPLE_FMT_FLTP,
                                      sample_rate, &layout, input_format, sample_rate, 0, nullptr),
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

void AudioEncoder::append(const AVFrame& frame, int first, int count) {
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
        ff::check(swr_convert_frame(resampler_.get(), planar.get(), &frame), "audio conversion");
        source = planar.get();
    }
    std::vector<const float*> planes(static_cast<std::size_t>(channels()));
    for (std::size_t c = 0; c < planes.size(); ++c) {
        planes[c] = reinterpret_cast<const float*>(source->extended_data[c]) + first;
    }
    append_planar(planes.data(), count);
}

void AudioEncoder::append_planar(const float* const* planes, int count) {
    if (count <= 0) {
        return;
    }
    // av_audio_fifo_write takes non-const pointers but only reads
    std::vector<void*> data(static_cast<std::size_t>(channels()));
    for (std::size_t c = 0; c < data.size(); ++c) {
        data[c] = const_cast<float*>(planes[c]);
    }
    ff::check(av_audio_fifo_write(fifo_.get(), data.data(), count), "audio FIFO");
}

void AudioEncoder::send_ready(bool final) {
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

// ------------------------------------------------------------------ packets

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

AVStream* add_encoded_stream(Output& output, const AVCodecContext& encoder) {
    AVStream* stream = output.add_stream();
    ff::check(avcodec_parameters_from_context(stream->codecpar, &encoder), "output stream");
    stream->time_base = encoder.time_base;
    if (encoder.codec != nullptr && writes_parameter_sets_in_stream(*encoder.codec)) {
        output.take_parameter_sets_from_stream(stream);
    }
    return stream;
}

std::vector<std::pair<std::string, std::string>> encoder_settings(const std::string& encoder) {
    if (encoder == "h264_vaapi") {
        // Mesa (RX 7800 XT, Mesa 26.2.6) misparses the headers FFmpeg passes it with the default
        // VBR: it hung at 2, 3, 4 and 6 Mbit/s for Full HD at 59.94 fps, wrote broken pictures
        // at 7 and 7.5 and ignored the bit rate at the others (about 2.4 Mbit/s at "8"). With
        // CBR and access unit delimiters, 1.5 to 40 Mbit/s all worked at the asked bit rate
        // (measured 2026-11-20).
        return {{"rc_mode", "CBR"}, {"aud", "1"}};
    }
    return {};
}

std::string encoder_label(const std::string& encoder) {
    std::string label = encoder;
    for (const auto& [option, value] : encoder_settings(encoder)) {
        label += std::format(" {}={}", option, value);
    }
    return label;
}

bool writes_parameter_sets_in_stream(const AVCodec& codec) {
    return codec.id == AV_CODEC_ID_H264 || codec.id == AV_CODEC_ID_HEVC;
}

} // namespace ttrally::cutting
