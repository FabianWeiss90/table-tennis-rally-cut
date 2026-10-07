// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_frame_decoder.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_video_stream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace ttrally::media {

namespace {

/// Decoding forward is cheaper than seeking for gaps up to this many frames.
constexpr std::int64_t kMaxForwardDecode = 90;

/// If a seek lands after the wanted frame, seek again this many frames earlier (then further).
constexpr std::array<std::int64_t, 3> kSeekBackoff{30, 240, 2400};

/// Output size: as requested, or derived from the height keeping the aspect ratio (never
/// upscaled). Planar YUV needs even sizes.
FrameSize output_size(int source_width, int source_height, const FrameOutput& output) {
    if (output.width > 0) {
        return {output.width, output.height};
    }
    const int height = std::min(output.height, source_height) / 2 * 2;
    const double aspect = static_cast<double>(source_width) / source_height;
    const int width = static_cast<int>(std::lround(height * aspect / 2.0)) * 2;
    return {width, height};
}

class FfmpegFrameDecoder final : public FrameDecoder {
  public:
    FfmpegFrameDecoder(const std::filesystem::path& path, VideoTimestamps timestamps,
                       DecodeBackend requested, const FrameOutput& output)
        : stream_(path, requested), timestamps_(std::move(timestamps)),
          size_(output_size(stream_.width(), stream_.height(), output)), layout_(output.layout) {
        if (timestamps_.pts.empty()) {
            throw MediaError(path.string() + " has no video frames");
        }
        const bool odd = size_.width % 2 != 0 || size_.height % 2 != 0;
        if (size_.width <= 0 || size_.height <= 0 || (layout_ == PixelLayout::Yuv420 && odd)) {
            throw std::invalid_argument("invalid output frame size");
        }
    }

    [[nodiscard]] DecodeBackend backend() const override { return stream_.backend(); }
    [[nodiscard]] std::int64_t frame_count() const override { return timestamps_.frame_count(); }
    [[nodiscard]] FrameSize frame_size() const override { return size_; }

    void decode(std::int64_t first, std::int64_t last, const FrameConsumer& consume) override {
        first = std::max<std::int64_t>(first, 0);
        last = std::min(last, frame_count() - 1);
        if (first > last) {
            return;
        }
        auto decoded = advance_to(first);
        while (decoded.frame) {
            if (!consume(convert(*decoded.frame, decoded.index)) || decoded.index >= last) {
                return;
            }
            decoded = next();
        }
    }

    void decode_selected(std::span<const std::int64_t> indices,
                         const FrameConsumer& consume) override {
        for (const std::int64_t target : indices) {
            if (target < 0 || target >= frame_count()) {
                continue;
            }
            const auto decoded = advance_to(target);
            if (!decoded.frame || !consume(convert(*decoded.frame, decoded.index))) {
                return;
            }
        }
    }

  private:
    struct Decoded {
        ff::FramePtr frame; ///< nullptr at the end of the video
        std::int64_t index = -1;
    };

    /// The next frame in presentation order.
    [[nodiscard]] Decoded next() {
        auto frame = stream_.next_frame();
        if (!frame) {
            next_index_.reset(); // end of file: the next request needs a seek
            return {};
        }
        const std::int64_t index = index_of(frame->best_effort_timestamp);
        next_index_ = index + 1;
        return {std::move(frame), index};
    }

    /// Positions the decoder on the target frame (forward decoding or seeking) and returns it.
    [[nodiscard]] Decoded advance_to(std::int64_t target) {
        if (!can_continue_to(target)) {
            seek_before(target, 0);
        }
        bool just_seeked = !next_index_;
        for (std::size_t attempt = 0;;) {
            Decoded decoded = next();
            if (!decoded.frame) {
                return {};
            }
            if (just_seeked && decoded.index > target && attempt < kSeekBackoff.size()) {
                seek_before(target, kSeekBackoff[attempt++]); // inexact seek landed too late
                continue;
            }
            just_seeked = false;
            if (decoded.index >= target) {
                return decoded;
            }
        }
    }

    [[nodiscard]] bool can_continue_to(std::int64_t target) const {
        return next_index_ && target >= *next_index_ && target - *next_index_ <= kMaxForwardDecode;
    }

    void seek_before(std::int64_t index, std::int64_t backoff) {
        const std::int64_t target = std::max<std::int64_t>(0, index - backoff);
        stream_.seek(timestamps_.pts[static_cast<std::size_t>(target)]);
        next_index_.reset();
    }

    /// Index of the timestamp nearest to pts in the sorted timestamp list.
    [[nodiscard]] std::int64_t index_of(std::int64_t pts) const {
        const auto& list = timestamps_.pts;
        const auto it = std::lower_bound(list.begin(), list.end(), pts);
        if (it == list.begin()) {
            return 0;
        }
        if (it == list.end() || (*it - pts) > (pts - *std::prev(it))) {
            return std::prev(it) - list.begin();
        }
        return it - list.begin();
    }

    [[nodiscard]] VideoFrame convert(const AVFrame& frame, std::int64_t index) {
        const AVPixelFormat target_format =
            layout_ == PixelLayout::Rgb24 ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_YUV420P;
        scaler_.reset(sws_getCachedContext(scaler_.release(), frame.width, frame.height,
                                           static_cast<AVPixelFormat>(frame.format), size_.width,
                                           size_.height, target_format, SWS_BILINEAR, nullptr,
                                           nullptr, nullptr));
        if (!scaler_) {
            throw MediaError("cannot create a scaler for this frame format");
        }
        VideoFrame output;
        output.index = index;
        output.time_s = static_cast<double>(timestamps_.pts[static_cast<std::size_t>(index)]) *
                        timestamps_.time_base.value();
        output.size = size_;
        output.layout = layout_;
        output.planes.resize(VideoFrame::bytes_for(size_, layout_));
        std::uint8_t* destination[3] = {output.planes.data(), nullptr, nullptr};
        int strides[3] = {3 * size_.width, 0, 0};
        if (layout_ == PixelLayout::Yuv420) {
            const auto luma =
                static_cast<std::size_t>(size_.width) * static_cast<std::size_t>(size_.height);
            destination[1] = output.planes.data() + luma;
            destination[2] = output.planes.data() + luma + luma / 4;
            strides[0] = size_.width;
            strides[1] = size_.width / 2;
            strides[2] = size_.width / 2;
        }
        sws_scale(scaler_.get(), frame.data, frame.linesize, 0, frame.height, destination, strides);
        return output;
    }

    ff::VideoStream stream_;
    VideoTimestamps timestamps_;
    FrameSize size_;
    PixelLayout layout_;
    std::optional<std::int64_t> next_index_; ///< Index the decoder delivers next, if known
    ff::SwsPtr scaler_;
};

} // namespace

std::unique_ptr<FrameDecoder> FfmpegFrameDecoderFactory::open(const std::filesystem::path& path,
                                                              const VideoTimestamps& timestamps,
                                                              DecodeBackend requested,
                                                              const FrameOutput& output) {
    return std::make_unique<FfmpegFrameDecoder>(path, timestamps, requested, output);
}

} // namespace ttrally::media
