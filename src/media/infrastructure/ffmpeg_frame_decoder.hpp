// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/frame_decoder.hpp"

namespace ttrally::media {

/// Opens FFmpeg-based frame-accurate decoders (hardware decoding if available, scaling with
/// libswscale).
class FfmpegFrameDecoderFactory final : public FrameDecoderFactory {
  public:
    [[nodiscard]] std::unique_ptr<FrameDecoder> open(const std::filesystem::path& path,
                                                     const VideoTimestamps& timestamps,
                                                     DecodeBackend requested,
                                                     const FrameOutput& output) override;
};

} // namespace ttrally::media
