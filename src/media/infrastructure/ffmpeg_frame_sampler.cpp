// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_frame_sampler.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ttrally::media {

namespace {

/// Upper bound of frames decoded after a seek before giving up.
constexpr int kMaxFramesPerSeek = 2000;

/// get_format callback: prefers the hardware format stored in ctx->opaque and falls back to
/// the first software format if the stream cannot be decoded in hardware.
AVPixelFormat select_pixel_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
    const AVPixelFormat hardware = *static_cast<const AVPixelFormat*>(ctx->opaque);
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == hardware) {
            return *format;
        }
    }
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            return *format;
        }
    }
    return AV_PIX_FMT_NONE;
}

/// Pixel format a decoder produces with the given hardware device type, if supported.
std::optional<AVPixelFormat> hardware_pixel_format(const AVCodec* codec, AVHWDeviceType type) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (config == nullptr) {
            return std::nullopt;
        }
        if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
            config->device_type == type) {
            return config->pix_fmt;
        }
    }
}

class FfmpegFrameSampler final : public FrameSampler {
  public:
    FfmpegFrameSampler(const std::filesystem::path& path, DecodeBackend requested) {
        open_stream(path);
        open_decoder(requested);
    }

    [[nodiscard]] DecodeBackend backend() const override { return backend_; }

    [[nodiscard]] std::optional<GrayImage> sample_gray(double t, int width, int height) override {
        if (width <= 0 || height <= 0) {
            throw std::invalid_argument("image size must be positive");
        }
        const double earliest = t - 0.5 * frame_duration_s_;
        seek(earliest);
        auto frame = decode_first_frame_from(earliest);
        if (!frame) {
            return std::nullopt;
        }
        return to_gray(frame.get(), width, height);
    }

  private:
    void open_stream(const std::filesystem::path& path) {
        format_ = ff::open_input(path);
        stream_index_ = av_find_best_stream(format_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index_ < 0) {
            throw MediaError(path.string() + " has no video stream");
        }
        for (unsigned i = 0; i < format_->nb_streams; ++i) {
            if (static_cast<int>(i) != stream_index_) {
                format_->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        stream_ = format_->streams[stream_index_];
        const AVRational rate =
            stream_->avg_frame_rate.num > 0 ? stream_->avg_frame_rate : stream_->r_frame_rate;
        if (rate.num > 0 && rate.den > 0) {
            frame_duration_s_ = av_q2d(av_inv_q(rate));
        }
    }

    void open_decoder(DecodeBackend requested) {
        std::vector<DecodeBackend> candidates;
        if (requested == DecodeBackend::Auto) {
            candidates = ff::platform_hardware_backends();
        } else if (requested != DecodeBackend::Cpu) {
            candidates = {requested};
        }
        for (const DecodeBackend candidate : candidates) {
            if (try_hardware(candidate)) {
                return;
            }
        }
        decoder_ = ff::open_decoder(stream_);
        backend_ = DecodeBackend::Cpu;
    }

    bool try_hardware(DecodeBackend candidate) {
        const ff::QuietLogScope quiet; // unsupported backends are expected
        const AVHWDeviceType type = ff::device_type(candidate);
        const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
        if (type == AV_HWDEVICE_TYPE_NONE || codec == nullptr) {
            return false;
        }
        const auto pixel_format = hardware_pixel_format(codec, type);
        AVBufferRef* device = nullptr;
        if (!pixel_format || av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0) {
            return false;
        }
        ff::BufferPtr device_ref(device);
        hardware_format_ = *pixel_format;
        try {
            decoder_ = ff::open_decoder(stream_, device_ref.get(), &select_pixel_format,
                                        &hardware_format_);
        } catch (const MediaError&) {
            hardware_format_ = AV_PIX_FMT_NONE;
            return false;
        }
        hardware_device_ = std::move(device_ref);
        backend_ = candidate;
        return true;
    }

    void seek(double time) {
        const auto timestamp =
            static_cast<std::int64_t>(std::floor(time / av_q2d(stream_->time_base)));
        ff::check(avformat_seek_file(format_.get(), stream_index_,
                                     std::numeric_limits<std::int64_t>::min(), timestamp,
                                     timestamp, 0),
                  "seek");
        avcodec_flush_buffers(decoder_.get());
    }

    /// Decodes until the first frame presented at or after `earliest`.
    ff::FramePtr decode_first_frame_from(double earliest) {
        auto packet = ff::make_packet();
        auto frame = ff::make_frame();
        bool flushed = false;
        for (int decoded = 0; decoded < kMaxFramesPerSeek;) {
            const int result = avcodec_receive_frame(decoder_.get(), frame.get());
            if (result == AVERROR_EOF || (result == AVERROR(EAGAIN) && flushed)) {
                return nullptr;
            }
            if (result == AVERROR(EAGAIN)) {
                flushed = !feed_next_packet(packet.get());
                continue;
            }
            ff::check(result, "video decoding");
            ++decoded;
            const double time = ff::to_seconds(frame->best_effort_timestamp, stream_->time_base);
            if (!std::isnan(time) && time >= earliest) {
                return frame;
            }
            av_frame_unref(frame.get());
        }
        return nullptr;
    }

    /// Sends the next packet of the stream to the decoder; returns false (after flushing the
    /// decoder) at the end of the file.
    bool feed_next_packet(AVPacket* packet) {
        if (av_read_frame(format_.get(), packet) < 0) {
            ff::check(avcodec_send_packet(decoder_.get(), nullptr), "decoder flush");
            return false;
        }
        if (packet->stream_index == stream_index_) {
            const int result = avcodec_send_packet(decoder_.get(), packet);
            if (result < 0 && result != AVERROR_INVALIDDATA) {
                av_packet_unref(packet);
                ff::check(result, "video decoding");
            }
        }
        av_packet_unref(packet);
        return true;
    }

    [[nodiscard]] GrayImage to_gray(AVFrame* frame, int width, int height) const {
        ff::FramePtr software;
        const AVFrame* source = frame;
        if (hardware_format_ != AV_PIX_FMT_NONE && frame->format == hardware_format_) {
            software = ff::make_frame();
            ff::check(av_hwframe_transfer_data(software.get(), frame, 0),
                      "transfer of hardware frame");
            source = software.get();
        }
        ff::SwsPtr scaler(sws_getContext(source->width, source->height,
                                         static_cast<AVPixelFormat>(source->format), width, height,
                                         AV_PIX_FMT_GRAY8, SWS_AREA, nullptr, nullptr, nullptr));
        if (!scaler) {
            throw MediaError("cannot create a scaler for this frame format");
        }
        GrayImage image;
        image.width = width;
        image.height = height;
        image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        std::uint8_t* destination[1] = {image.pixels.data()};
        const int destination_stride[1] = {width};
        sws_scale(scaler.get(), source->data, source->linesize, 0, source->height, destination,
                  destination_stride);
        return image;
    }

    ff::FormatPtr format_;
    ff::CodecPtr decoder_;
    ff::BufferPtr hardware_device_;
    AVPixelFormat hardware_format_ = AV_PIX_FMT_NONE;
    AVStream* stream_ = nullptr;
    int stream_index_ = -1;
    DecodeBackend backend_ = DecodeBackend::Cpu;
    double frame_duration_s_ = 1.0 / 30.0;
};

} // namespace

std::unique_ptr<FrameSampler> FfmpegFrameSamplerFactory::open(const std::filesystem::path& path,
                                                              DecodeBackend requested) {
    return std::make_unique<FfmpegFrameSampler>(path, requested);
}

} // namespace ttrally::media
