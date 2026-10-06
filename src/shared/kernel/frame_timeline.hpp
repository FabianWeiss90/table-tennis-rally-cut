// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/kernel/rational.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ttrally {

/// Maps presentation times (seconds on the file's timeline) to 0-based frame indices of the
/// decoded video stream and back.
///
/// Constant frame rate: frame = round((t - start_time) * fps).
/// Variable frame rate: nearest entry of the sorted list of frame presentation times.
class FrameTimeline {
  public:
    /// Constant frame rate with the given exact rate and the presentation time of frame 0.
    static FrameTimeline constant(Rational fps, double start_time_s);

    /// Variable frame rate from the presentation times of all frames (seconds, any order).
    static FrameTimeline variable(std::vector<double> frame_times_s, Rational nominal_fps);

    /// Builds a timeline from the packet PTS values of a video stream. Uses the constant model if
    /// every PTS lies within a tenth of a frame (at least one tick) of the nominal grid,
    /// the PTS list otherwise.
    static FrameTimeline from_pts(std::span<const std::int64_t> pts, Rational time_base,
                                  Rational nominal_fps);

    [[nodiscard]] bool is_constant() const noexcept { return times_.empty(); }
    [[nodiscard]] Rational nominal_fps() const noexcept { return fps_; }
    [[nodiscard]] double start_time() const noexcept { return start_; }

    /// Number of frames if known (variable frame rate only), otherwise -1.
    [[nodiscard]] std::int64_t frame_count() const noexcept {
        return is_constant() ? -1 : static_cast<std::int64_t>(times_.size());
    }

    /// Index of the frame whose presentation time is nearest to t (ties: the earlier frame).
    /// For variable frame rate the result is clamped to the valid frame range.
    [[nodiscard]] std::int64_t time_to_frame(double t) const;

    /// Presentation time of a frame.
    [[nodiscard]] double frame_to_time(std::int64_t frame) const;

  private:
    Rational fps_{};
    double start_ = 0.0;
    std::vector<double> times_;
};

} // namespace ttrally
