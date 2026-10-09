// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "detection/domain/frame_grid.hpp"

#include <algorithm>
#include <stdexcept>

namespace ttrally::detection {

FrameGrid::FrameGrid(std::vector<std::int64_t> row_frames, std::int64_t video_frame_count)
    : row_frames_(std::move(row_frames)), video_frame_count_(video_frame_count) {
    if (!std::ranges::is_sorted(row_frames_)) {
        throw std::invalid_argument("the frames of the feature rows must be ascending");
    }
}

FrameSpan FrameGrid::frames_of(const RowSegment& segment) const {
    const auto first = static_cast<std::size_t>(segment.first);
    const auto next = static_cast<std::size_t>(segment.last) + 1;
    const std::int64_t start = row_frames_.at(first);
    const std::int64_t end =
        next < row_frames_.size() ? row_frames_[next] - 1 : video_frame_count_ - 1;
    return {start, std::max(start, end)};
}

std::vector<bool> FrameGrid::rows_inside(std::span<const FrameSpan> spans) const {
    std::vector<bool> inside(row_frames_.size(), false);
    for (std::size_t row = 0; row < row_frames_.size(); ++row) {
        const std::int64_t frame = row_frames_[row];
        inside[row] = std::ranges::any_of(spans, [frame](const FrameSpan& span) {
            return frame >= span.start_frame && frame <= span.end_frame;
        });
    }
    return inside;
}

} // namespace ttrally::detection
