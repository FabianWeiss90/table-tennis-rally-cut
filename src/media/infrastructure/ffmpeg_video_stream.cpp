// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_video_stream.hpp"

#include "media/application/media_error.hpp"

#include <limits>
#include <optional>

namespace ttrally::media::ff {

namespace {

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

} // namespace

VideoStream::VideoStream(const std::filesystem::path& path, DecodeBackend requested)
    : format_(open_input(path)), packet_(make_packet()) {
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
    open_decoder(requested);
}

void VideoStream::open_decoder(DecodeBackend requested) {
    std::vector<DecodeBackend> candidates;
    if (requested == DecodeBackend::Auto) {
        candidates = platform_hardware_backends();
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

bool VideoStream::try_hardware(DecodeBackend candidate) {
    const QuietLogScope quiet; // unsupported backends are expected
    const AVHWDeviceType type = device_type(candidate);
    const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (type == AV_HWDEVICE_TYPE_NONE || codec == nullptr) {
        return false;
    }
    const auto pixel_format = hardware_pixel_format(codec, type);
    AVBufferRef* device = nullptr;
    if (!pixel_format || av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0) {
        return false;
    }
    BufferPtr device_ref(device);
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

void VideoStream::seek(std::int64_t timestamp) {
    check(avformat_seek_file(format_.get(), stream_index_, std::numeric_limits<std::int64_t>::min(),
                             timestamp, timestamp, 0),
          "seek");
    avcodec_flush_buffers(decoder_.get());
    flushed_ = false;
}

FramePtr VideoStream::next_frame() {
    auto frame = make_frame();
    for (;;) {
        const int result = avcodec_receive_frame(decoder_.get(), frame.get());
        if (result == AVERROR_EOF || (result == AVERROR(EAGAIN) && flushed_)) {
            return nullptr;
        }
        if (result == AVERROR(EAGAIN)) {
            flushed_ = !feed_next_packet();
            continue;
        }
        check(result, "video decoding");
        return to_software(std::move(frame));
    }
}

bool VideoStream::feed_next_packet() {
    if (av_read_frame(format_.get(), packet_.get()) < 0) {
        check(avcodec_send_packet(decoder_.get(), nullptr), "decoder flush");
        return false;
    }
    if (packet_->stream_index == stream_index_) {
        const int result = avcodec_send_packet(decoder_.get(), packet_.get());
        if (result < 0 && result != AVERROR_INVALIDDATA) {
            av_packet_unref(packet_.get());
            check(result, "video decoding");
        }
    }
    av_packet_unref(packet_.get());
    return true;
}

FramePtr VideoStream::to_software(FramePtr frame) const {
    if (hardware_format_ == AV_PIX_FMT_NONE || frame->format != hardware_format_) {
        return frame;
    }
    auto software = make_frame();
    check(av_hwframe_transfer_data(software.get(), frame.get(), 0), "transfer of hardware frame");
    check(av_frame_copy_props(software.get(), frame.get()), "frame properties");
    return software;
}

} // namespace ttrally::media::ff
