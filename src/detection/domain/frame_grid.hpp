// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "detection/domain/row_segment.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ttrally::detection {

/// A span of frames of the original, end inclusive.
struct FrameSpan {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0;
};

/// The feature grid of a video: the frame of the original used for each row. Translates between
/// rows and frames exactly as training does (a row belongs to a span if its frame lies inside).
class FrameGrid {
  public:
    FrameGrid(std::vector<std::int64_t> row_frames, std::int64_t video_frame_count);

    [[nodiscard]] std::size_t rows() const noexcept { return row_frames_.size(); }

    /// Frames covered by a segment: from the frame of its first row up to the frame before the
    /// next row (or the end of the video).
    [[nodiscard]] FrameSpan frames_of(const RowSegment& segment) const;
    /// Rows whose frame lies inside one of the spans.
    [[nodiscard]] std::vector<bool> rows_inside(std::span<const FrameSpan> spans) const;

  private:
    std::vector<std::int64_t> row_frames_;
    std::int64_t video_frame_count_;
};

} // namespace ttrally::detection
