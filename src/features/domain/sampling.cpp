// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/domain/sampling.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ttrally::features {

std::vector<Sample> plan_samples(const FrameTimeline& timeline, std::int64_t frame_count,
                                 double rate_hz) {
    if (rate_hz <= 0.0 || frame_count <= 0) {
        throw std::invalid_argument("sampling needs a positive rate and at least one frame");
    }
    const double first = timeline.frame_to_time(0);
    const double last = timeline.frame_to_time(frame_count - 1);
    const auto count = static_cast<std::int64_t>(std::floor((last - first) * rate_hz)) + 1;

    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (std::int64_t k = 0; k < count; ++k) {
        const double time = first + static_cast<double>(k) / rate_hz;
        const std::int64_t frame =
            std::clamp<std::int64_t>(timeline.time_to_frame(time), 0, frame_count - 1);
        samples.push_back({time, frame});
    }
    return samples;
}

std::vector<std::int64_t> frames_to_decode(const std::vector<Sample>& samples) {
    std::vector<std::int64_t> frames;
    frames.reserve(samples.size());
    for (const Sample& sample : samples) {
        frames.push_back(sample.frame);
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return frames;
}

} // namespace ttrally::features
