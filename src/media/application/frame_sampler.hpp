// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/decode_backend.hpp"
#include "media/domain/gray_image.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace ttrally::media {

/// Port: decodes single video frames at arbitrary times.
class FrameSampler {
  public:
    virtual ~FrameSampler() = default;

    /// Backend actually used for decoding.
    [[nodiscard]] virtual DecodeBackend backend() const = 0;

    /// The frame shown at time t (seconds on the file's timeline), scaled to a greyscale image.
    /// Returns nullopt if there is no frame at that time.
    [[nodiscard]] virtual std::optional<GrayImage> sample_gray(double t, int width,
                                                               int height) = 0;
};

/// Port: opens frame samplers. With DecodeBackend::Auto, hardware backends are tried first and
/// software decoding is the fallback.
class FrameSamplerFactory {
  public:
    virtual ~FrameSamplerFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<FrameSampler> open(const std::filesystem::path& path,
                                                             DecodeBackend requested) = 0;
};

} // namespace ttrally::media
