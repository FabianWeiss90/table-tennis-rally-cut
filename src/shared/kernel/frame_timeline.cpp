// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/kernel/frame_timeline.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ttrally {

FrameTimeline FrameTimeline::constant(Rational fps, double start_time_s) {
    if (!fps.positive()) {
        throw std::invalid_argument("constant frame rate must be positive");
    }
    FrameTimeline timeline;
    timeline.fps_ = fps;
    timeline.start_ = start_time_s;
    return timeline;
}

FrameTimeline FrameTimeline::variable(std::vector<double> frame_times_s, Rational nominal_fps) {
    if (frame_times_s.empty()) {
        throw std::invalid_argument("variable frame rate timeline needs at least one frame");
    }
    std::sort(frame_times_s.begin(), frame_times_s.end());
    FrameTimeline timeline;
    timeline.fps_ = nominal_fps;
    timeline.start_ = frame_times_s.front();
    timeline.times_ = std::move(frame_times_s);
    return timeline;
}

FrameTimeline FrameTimeline::from_pts(std::span<const std::int64_t> pts, Rational time_base,
                                      Rational nominal_fps) {
    if (pts.empty()) {
        throw std::invalid_argument("video stream has no frames with timestamps");
    }
    if (!time_base.positive()) {
        throw std::invalid_argument("time base must be positive");
    }
    std::vector<std::int64_t> sorted(pts.begin(), pts.end());
    std::sort(sorted.begin(), sorted.end());

    if (nominal_fps.positive()) {
        // Frame duration in ticks: (1 / fps) / time_base
        const double ticks_per_frame =
            static_cast<double>(nominal_fps.den) * static_cast<double>(time_base.den) /
            (static_cast<double>(nominal_fps.num) * static_cast<double>(time_base.num));
        const double tolerance = std::max(1.0, 0.1 * ticks_per_frame);
        bool constant_rate = true;
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            const double expected = static_cast<double>(i) * ticks_per_frame;
            const auto actual = static_cast<double>(sorted[i] - sorted.front());
            if (std::abs(actual - expected) > tolerance) {
                constant_rate = false;
                break;
            }
        }
        if (constant_rate) {
            return constant(nominal_fps, static_cast<double>(sorted.front()) * time_base.value());
        }
    }

    std::vector<double> times;
    times.reserve(sorted.size());
    for (const std::int64_t value : sorted) {
        times.push_back(static_cast<double>(value) * static_cast<double>(time_base.num) /
                        static_cast<double>(time_base.den));
    }
    return variable(std::move(times), nominal_fps);
}

std::int64_t FrameTimeline::time_to_frame(double t) const {
    if (is_constant()) {
        const double position = (t - start_) * static_cast<double>(fps_.num) /
                                static_cast<double>(fps_.den);
        // Round half down so that a time exactly between two frames maps to the earlier one.
        return static_cast<std::int64_t>(std::ceil(position - 0.5));
    }
    const auto it = std::lower_bound(times_.begin(), times_.end(), t);
    if (it == times_.begin()) {
        return 0;
    }
    if (it == times_.end()) {
        return static_cast<std::int64_t>(times_.size()) - 1;
    }
    const auto after = static_cast<std::int64_t>(it - times_.begin());
    // The later frame wins only if it is clearly nearer (ties within 1 ns go to the earlier one).
    constexpr double kTieTolerance = 1e-9;
    return (*it - t) < (t - *(it - 1)) - kTieTolerance ? after : after - 1;
}

double FrameTimeline::frame_to_time(std::int64_t frame) const {
    if (is_constant()) {
        return start_ + static_cast<double>(frame) * static_cast<double>(fps_.den) /
                            static_cast<double>(fps_.num);
    }
    if (frame < 0 || frame >= static_cast<std::int64_t>(times_.size())) {
        throw std::out_of_range("frame index outside of the video");
    }
    return times_[static_cast<std::size_t>(frame)];
}

} // namespace ttrally
