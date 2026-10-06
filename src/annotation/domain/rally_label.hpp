// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ttrally::annotation {

/// A rally as annotated by a person, in frames of the original at its native frame rate.
///
/// Start: first frame in which the ball visibly leaves the palm during the service toss.
/// End: frame in which the point is decided. Bouncing the ball before serving is not part of the
/// rally; a toss that is caught again counts as a (very short) rally flagged aborted_toss.
struct RallyLabel {
    std::int64_t start_frame = 0;
    std::int64_t end_frame = 0; ///< inclusive
    std::optional<std::int64_t> serve_contact_frame;
    bool aborted_toss = false;
    std::string notes;

    [[nodiscard]] bool contains(std::int64_t frame) const noexcept {
        return frame >= start_frame && frame <= end_frame;
    }
    [[nodiscard]] bool overlaps(std::int64_t first, std::int64_t last) const noexcept {
        return start_frame <= last && first <= end_frame;
    }
    [[nodiscard]] std::int64_t frame_count() const noexcept { return end_frame - start_frame + 1; }
};

} // namespace ttrally::annotation
