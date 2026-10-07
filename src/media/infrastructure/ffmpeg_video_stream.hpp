// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Internal: decodes the best video stream of a file with optional hardware acceleration. Shared by the frame sampler and the frame decoder.

#pragma once

#include "media/infrastructure/ffmpeg_api.hpp"

#include <cstdint>
#include <filesystem>

namespace ttrally::media::ff {

class VideoStream {
  public:
    /// Opens the best video stream. With DecodeBackend::Auto the platform's hardware backends are
    /// tried in order; if none works, software decoding is used.
    VideoStream(const std::filesystem::path& path, DecodeBackend requested);
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    [[nodiscard]] DecodeBackend backend() const noexcept { return backend_; }
    [[nodiscard]] AVRational time_base() const noexcept { return stream_->time_base; }
    [[nodiscard]] int width() const noexcept { return stream_->codecpar->width; }
    [[nodiscard]] int height() const noexcept { return stream_->codecpar->height; }
    [[nodiscard]] double frame_duration_s() const noexcept { return frame_duration_s_; }

    /// Seeks to the keyframe at or before `timestamp` (stream time base).
    void seek(std::int64_t timestamp);

    /// Next frame in presentation order, possibly still in GPU memory; nullptr at the end.
    [[nodiscard]] FramePtr next_frame();

    /// The frame in system memory. Copying a frame from the GPU is expensive, so callers do
    /// this only for frames they actually use.
    [[nodiscard]] FramePtr to_system_memory(FramePtr frame) const;

  private:
    void open_decoder(DecodeBackend requested);
    bool try_hardware(DecodeBackend candidate);
    /// Sends the next packet to the decoder; returns false (after flushing) at the end.
    bool feed_next_packet();

    FormatPtr format_;
    CodecPtr decoder_;
    BufferPtr hardware_device_;
    PacketPtr packet_;
    AVPixelFormat hardware_format_ = AV_PIX_FMT_NONE;
    AVStream* stream_ = nullptr;
    int stream_index_ = -1;
    DecodeBackend backend_ = DecodeBackend::Cpu;
    double frame_duration_s_ = 1.0 / 30.0;
    bool flushed_ = false;
};

} // namespace ttrally::media::ff
