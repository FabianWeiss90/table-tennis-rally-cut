// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>
#include <string>

namespace ttrally::annotation {

/// A stretch of the video that training leaves out, e.g. a rally whose beginning is missing
/// because the recording started late. Not a rally: it has no rally id, flags or serve contact,
/// only start, end (inclusive) and an optional note.
struct IgnoredSection {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0; ///< inclusive
    std::string notes;

    [[nodiscard]] bool contains(std::int64_t frame) const noexcept {
        return frame >= start_frame && frame <= end_frame;
    }
    [[nodiscard]] bool overlaps(std::int64_t first, std::int64_t last) const noexcept {
        return start_frame <= last && first <= end_frame;
    }
};

} // namespace ttrally::annotation
