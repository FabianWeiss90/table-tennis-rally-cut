// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_frame_sampler.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_video_stream.hpp"

#include <cmath>
#include <stdexcept>

namespace ttrally::media {

namespace {

/// Upper bound of frames decoded after a seek before giving up.
constexpr int kMaxFramesPerSeek = 2000;

class FfmpegFrameSampler final : public FrameSampler {
  public:
    FfmpegFrameSampler(const std::filesystem::path& path, DecodeBackend requested)
        : stream_(path, requested) {}

    [[nodiscard]] DecodeBackend backend() const override { return stream_.backend(); }

    [[nodiscard]] std::optional<GrayImage> sample_gray(double t, int width, int height) override {
        if (width <= 0 || height <= 0) {
            throw std::invalid_argument("image size must be positive");
        }
        const double earliest = t - 0.5 * stream_.frame_duration_s();
        stream_.seek(static_cast<std::int64_t>(std::floor(earliest / av_q2d(stream_.time_base()))));
        for (int decoded = 0; decoded < kMaxFramesPerSeek; ++decoded) {
            auto frame = stream_.next_frame();
            if (!frame) {
                return std::nullopt;
            }
            const double time = ff::to_seconds(frame->best_effort_timestamp, stream_.time_base());
            if (!std::isnan(time) && time >= earliest) {
                return to_gray(*frame, width, height);
            }
        }
        return std::nullopt;
    }

  private:
    [[nodiscard]] static GrayImage to_gray(const AVFrame& frame, int width, int height) {
        ff::SwsPtr scaler(sws_getContext(frame.width, frame.height,
                                         static_cast<AVPixelFormat>(frame.format), width, height,
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
        sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height, destination,
                  destination_stride);
        return image;
    }

    ff::VideoStream stream_;
};

} // namespace

std::unique_ptr<FrameSampler> FfmpegFrameSamplerFactory::open(const std::filesystem::path& path,
                                                              DecodeBackend requested) {
    return std::make_unique<FfmpegFrameSampler>(path, requested);
}

} // namespace ttrally::media
