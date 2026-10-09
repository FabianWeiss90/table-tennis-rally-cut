// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "detection/domain/row_segment.hpp"

#include <algorithm>

namespace ttrally::detection {

double RowSegment::iou(const RowSegment& other) const noexcept {
    const std::int64_t intersection =
        std::min(last, other.last) - std::max(first, other.first) + 1;
    if (intersection <= 0) {
        return 0.0;
    }
    return static_cast<double>(intersection) /
           static_cast<double>(length() + other.length() - intersection);
}

std::vector<RowSegment> segments_of(const std::vector<bool>& mask) {
    std::vector<RowSegment> segments;
    for (std::size_t row = 0; row < mask.size(); ++row) {
        if (!mask[row]) {
            continue;
        }
        const auto first = static_cast<std::int64_t>(row);
        while (row + 1 < mask.size() && mask[row + 1]) {
            ++row;
        }
        segments.push_back({first, static_cast<std::int64_t>(row)});
    }
    return segments;
}

} // namespace ttrally::detection
