// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/video_frame.hpp"

#include <array>
#include <vector>

namespace ttrally::features {

/// What the image model expects: input size and per-channel normalisation of RGB values in
/// [0, 1] as (value - mean) / stddev.
struct ModelInputSpec {
    media::FrameSize size;
    std::array<float, 3> mean{};
    std::array<float, 3> stddev{};
};

/// Appends an RGB frame (exactly spec.size) to a batch in planar CHW layout, normalised.
/// Throws std::invalid_argument if the frame does not match the spec.
void append_model_input(const media::VideoFrame& frame, const ModelInputSpec& spec,
                        std::vector<float>& batch);

} // namespace ttrally::features
