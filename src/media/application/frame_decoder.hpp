// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/decode_backend.hpp"
#include "media/domain/video_frame.hpp"
#include "media/domain/video_timestamps.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace ttrally::media {

/// Port: frame-accurate decoding of a video. Frames are identified by their index in the sorted
/// list of presentation timestamps, the same numbering used by alignment and labels.
class FrameDecoder {
  public:
    /// Receives decoded frames; returning false stops decoding.
    using FrameConsumer = std::function<bool(VideoFrame&&)>;

    virtual ~FrameDecoder() = default;

    [[nodiscard]] virtual DecodeBackend backend() const = 0;
    [[nodiscard]] virtual std::int64_t frame_count() const = 0;
    [[nodiscard]] virtual FrameSize frame_size() const = 0;

    /// Decodes the frames first..last (inclusive, clamped to the video) in presentation order and
    /// passes each to `consume`. Continues from the current position if possible, otherwise
    /// seeks to the keyframe before `first`.
    virtual void decode(std::int64_t first, std::int64_t last, const FrameConsumer& consume) = 0;
};

/// Port: opens frame decoders.
class FrameDecoderFactory {
  public:
    virtual ~FrameDecoderFactory() = default;

    /// Opens the video stream whose frame timestamps are given. Frames are scaled to
    /// `output_height` (keeping the aspect ratio, never upscaled).
    [[nodiscard]] virtual std::unique_ptr<FrameDecoder>
    open(const std::filesystem::path& path, const VideoTimestamps& timestamps,
         DecodeBackend requested, int output_height) = 0;
};

} // namespace ttrally::media
