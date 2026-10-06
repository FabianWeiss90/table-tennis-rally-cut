// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_api.hpp"

#include "media/application/media_error.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace ttrally::media::ff {

std::vector<DecodeBackend> platform_hardware_backends() {
#ifdef _WIN32
    return {DecodeBackend::D3d12va, DecodeBackend::D3d11va, DecodeBackend::Cuda};
#else
    return {DecodeBackend::Vaapi, DecodeBackend::Cuda, DecodeBackend::Vulkan};
#endif
}

AVHWDeviceType device_type(DecodeBackend backend) {
    // Looked up by name so that the code also builds against FFmpeg versions that lack a device
    // type (e.g. D3D12VA before FFmpeg 7).
    const std::string name(to_string(backend));
    return av_hwdevice_find_type_by_name(name.c_str());
}

std::string error_string(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

void check(int code, std::string_view what) {
    if (code < 0) {
        throw MediaError(std::string(what) + ": " + error_string(code));
    }
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return {u8.begin(), u8.end()};
}

FormatPtr open_input(const std::filesystem::path& path) {
    AVFormatContext* raw = nullptr;
    const std::string name = path_to_utf8(path);
    const int result = avformat_open_input(&raw, name.c_str(), nullptr, nullptr);
    if (result < 0) {
        throw MediaError("cannot open " + path.string() + ": " + error_string(result));
    }
    FormatPtr ctx(raw);
    check(avformat_find_stream_info(ctx.get(), nullptr),
          "cannot read stream information of " + path.string());
    return ctx;
}

FramePtr make_frame() {
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw MediaError("out of memory (av_frame_alloc)");
    }
    return frame;
}

PacketPtr make_packet() {
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        throw MediaError("out of memory (av_packet_alloc)");
    }
    return packet;
}

double to_seconds(std::int64_t timestamp, AVRational time_base) noexcept {
    if (timestamp == AV_NOPTS_VALUE) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return static_cast<double>(timestamp) * av_q2d(time_base);
}

CodecPtr open_decoder(const AVStream* stream, AVBufferRef* hw_device,
                      AVPixelFormat (*get_format)(AVCodecContext*, const AVPixelFormat*),
                      void* opaque) {
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        throw MediaError(std::string("no decoder available for codec ") +
                         avcodec_get_name(stream->codecpar->codec_id));
    }
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        throw MediaError("out of memory (avcodec_alloc_context3)");
    }
    check(avcodec_parameters_to_context(ctx.get(), stream->codecpar), "decoder parameters");
    ctx->pkt_timebase = stream->time_base;
    if (hw_device != nullptr) {
        ctx->hw_device_ctx = av_buffer_ref(hw_device);
        ctx->get_format = get_format;
        ctx->opaque = opaque;
    }
    check(avcodec_open2(ctx.get(), codec, nullptr),
          std::string("cannot open decoder ") + codec->name);
    return ctx;
}

} // namespace ttrally::media::ff
