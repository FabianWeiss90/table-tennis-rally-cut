// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/ffmpeg_video_preview.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_video_stream.hpp"

#include <algorithm>
#include <cmath>

namespace ttrally::launcher {

namespace ff = media::ff;

namespace {

/// Frames decoded after a seek at most before giving up (camera keyframes are a few s apart).
constexpr int kMaxFramesAfterSeek = 1200;

media::VideoFrame scaled(const AVFrame& frame, int height, double time_s) {
    const int out_height = std::max(2, std::min(height, frame.height) / 2 * 2);
    const int out_width = std::max(2, static_cast<int>(std::lround(static_cast<double>(frame.width) *
                                                                   out_height / frame.height /
                                                                   2.0)) *
                                          2);
    ff::SwsPtr scaler(sws_getContext(frame.width, frame.height,
                                     static_cast<AVPixelFormat>(frame.format), out_width,
                                     out_height, AV_PIX_FMT_YUV420P, SWS_AREA, nullptr, nullptr,
                                     nullptr));
    if (!scaler) {
        throw media::MediaError("cannot scale the preview frame");
    }
    const bool full_range = frame.color_range == AVCOL_RANGE_JPEG ||
                            frame.format == AV_PIX_FMT_YUVJ420P;
    const int* coefficients = sws_getCoefficients(frame.colorspace);
    sws_setColorspaceDetails(scaler.get(), coefficients, full_range ? 1 : 0, coefficients, 0, 0,
                             1 << 16, 1 << 16); // the renderer expects limited range
    media::VideoFrame result;
    result.time_s = time_s;
    result.size = {out_width, out_height};
    result.layout = media::PixelLayout::Yuv420;
    result.planes.resize(media::VideoFrame::bytes_for(result.size));
    const auto luma = static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height);
    std::uint8_t* planes[3] = {result.planes.data(), result.planes.data() + luma,
                               result.planes.data() + luma + luma / 4};
    const int strides[3] = {out_width, out_width / 2, out_width / 2};
    sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height, planes, strides);
    return result;
}

} // namespace

std::optional<media::VideoFrame> FfmpegVideoPreview::frame_at(const std::filesystem::path& video,
                                                              double fraction, int height) {
    try {
        const media::MediaInfo info = media::FfmpegMediaProbe().probe(video);
        if (!info.video) {
            return std::nullopt;
        }
        const double duration =
            std::isnan(info.video->duration_s) ? info.duration_s : info.video->duration_s;
        const double target = info.video->start_time_s +
                              std::clamp(fraction, 0.0, 1.0) *
                                  (std::isnan(duration) ? 0.0 : std::max(0.0, duration - 1.0));
        ff::VideoStream stream(video, media::DecodeBackend::Cpu);
        const double time_base = av_q2d(stream.time_base());
        stream.seek(static_cast<std::int64_t>(std::floor(target / time_base)));
        ff::FramePtr last;
        double last_time = 0.0;
        for (int decoded = 0; decoded < kMaxFramesAfterSeek; ++decoded) {
            auto frame = stream.next_frame();
            if (!frame) {
                break; // the end: show the last frame
            }
            last_time = static_cast<double>(frame->best_effort_timestamp) * time_base;
            last = std::move(frame);
            if (last_time >= target - stream.frame_duration_s() / 2) {
                break;
            }
        }
        if (!last) {
            return std::nullopt;
        }
        return scaled(*stream.to_system_memory(std::move(last)), height, last_time);
    } catch (const std::exception&) {
        return std::nullopt; // the preview is optional; commands report real errors
    }
}

} // namespace ttrally::launcher
