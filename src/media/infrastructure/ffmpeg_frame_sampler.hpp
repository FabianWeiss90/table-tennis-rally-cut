// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/frame_sampler.hpp"

namespace ttrally::media {

/// Opens FFmpeg-based frame samplers with optional hardware decoding.
class FfmpegFrameSamplerFactory final : public FrameSamplerFactory {
  public:
    [[nodiscard]] std::unique_ptr<FrameSampler> open(const std::filesystem::path& path,
                                                     DecodeBackend requested) override;
};

} // namespace ttrally::media
