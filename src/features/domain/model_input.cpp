// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/domain/model_input.hpp"

#include <stdexcept>

namespace ttrally::features {

void append_model_input(const media::VideoFrame& frame, const ModelInputSpec& spec,
                        std::vector<float>& batch) {
    if (frame.layout != media::PixelLayout::Rgb24 || frame.size.width != spec.size.width ||
        frame.size.height != spec.size.height) {
        throw std::invalid_argument("frame does not match the model input");
    }
    const auto pixels = static_cast<std::size_t>(spec.size.width) *
                        static_cast<std::size_t>(spec.size.height);
    const std::size_t offset = batch.size();
    batch.resize(offset + 3 * pixels);
    const auto rgb = frame.rgb();
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float scale = 1.0F / (255.0F * spec.stddev[channel]);
        const float shift = spec.mean[channel] / spec.stddev[channel];
        float* plane = batch.data() + offset + channel * pixels;
        for (std::size_t i = 0; i < pixels; ++i) {
            plane[i] = static_cast<float>(rgb[3 * i + channel]) * scale - shift;
        }
    }
}

} // namespace ttrally::features
