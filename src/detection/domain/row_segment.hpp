// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ttrally::detection {

/// A rally on the feature grid (one row per sample, 10 per second by default), end inclusive.
struct RowSegment {
    std::int64_t first = 0;
    std::int64_t last = 0;

    [[nodiscard]] std::int64_t length() const noexcept { return last - first + 1; }
    /// Intersection over union of the two row ranges.
    [[nodiscard]] double iou(const RowSegment& other) const noexcept;

    friend bool operator==(const RowSegment&, const RowSegment&) = default;
};

/// Runs of true values in a per-row mask.
[[nodiscard]] std::vector<RowSegment> segments_of(const std::vector<bool>& mask);

} // namespace ttrally::detection
