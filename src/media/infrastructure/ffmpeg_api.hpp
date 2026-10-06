// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Internal helpers around the FFmpeg C API. Only included by .cpp files of the media
// infrastructure so that FFmpeg headers do not leak into the rest of the code base.

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "media/domain/decode_backend.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::media::ff {

struct FormatCloser {
    void operator()(AVFormatContext* ctx) const noexcept { avformat_close_input(&ctx); }
};
struct CodecFreer {
    void operator()(AVCodecContext* ctx) const noexcept { avcodec_free_context(&ctx); }
};
struct FrameFreer {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
struct PacketFreer {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
struct SwrFreer {
    void operator()(SwrContext* ctx) const noexcept { swr_free(&ctx); }
};
struct SwsFreer {
    void operator()(SwsContext* ctx) const noexcept { sws_freeContext(ctx); }
};
struct BufferUnref {
    void operator()(AVBufferRef* ref) const noexcept { av_buffer_unref(&ref); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFreer>;
using FramePtr = std::unique_ptr<AVFrame, FrameFreer>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFreer>;
using SwrPtr = std::unique_ptr<SwrContext, SwrFreer>;
using SwsPtr = std::unique_ptr<SwsContext, SwsFreer>;
using BufferPtr = std::unique_ptr<AVBufferRef, BufferUnref>;

/// Silences FFmpeg's console output while it exists, unless verbose logging is enabled.
/// Used while probing optional features (e.g. hardware devices) whose failure is expected.
class QuietLogScope {
  public:
    QuietLogScope() noexcept : saved_(av_log_get_level()) {
        if (saved_ <= AV_LOG_ERROR) {
            av_log_set_level(AV_LOG_QUIET);
        }
    }
    ~QuietLogScope() { av_log_set_level(saved_); }
    QuietLogScope(const QuietLogScope&) = delete;
    QuietLogScope& operator=(const QuietLogScope&) = delete;

  private:
    int saved_;
};

/// Human-readable message for an FFmpeg error code.
[[nodiscard]] std::string error_string(int code);

/// Hardware backends in the order `auto` tries them on this platform.
[[nodiscard]] std::vector<DecodeBackend> platform_hardware_backends();

/// FFmpeg device type of a backend; AV_HWDEVICE_TYPE_NONE if this FFmpeg build lacks it.
[[nodiscard]] AVHWDeviceType device_type(DecodeBackend backend);

/// Throws MediaError("<what>: <ffmpeg message>") if code is negative.
void check(int code, std::string_view what);

/// UTF-8 representation of a path, as expected by FFmpeg on all platforms.
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

/// Opens a media file and reads its stream information. Throws MediaError on failure.
[[nodiscard]] FormatPtr open_input(const std::filesystem::path& path);

[[nodiscard]] FramePtr make_frame();
[[nodiscard]] PacketPtr make_packet();

/// Converts a timestamp to seconds; returns NaN for AV_NOPTS_VALUE.
[[nodiscard]] double to_seconds(std::int64_t timestamp, AVRational time_base) noexcept;

/// Opens a decoder for a stream. If hw_device is set, it is attached to the codec context.
[[nodiscard]] CodecPtr open_decoder(const AVStream* stream, AVBufferRef* hw_device = nullptr,
                                    AVPixelFormat (*get_format)(AVCodecContext*,
                                                                const AVPixelFormat*) = nullptr,
                                    void* opaque = nullptr);

} // namespace ttrally::media::ff
