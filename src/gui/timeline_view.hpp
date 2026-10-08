// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "annotation/application/annotation_session.hpp"

#include <cstdint>
#include <optional>

namespace ttrally::gui {

/// Frame range shown by the timeline.
struct TimelineRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

/// Timeline of the current review item: the alignment hint, saved rallies and ignored sections,
/// the draft marks and the current frame. Clicking or dragging selects a frame.
class TimelineView {
  public:
    static constexpr float kHeight = 56.0F;

    /// Draws the timeline over the full available width. Returns the frame the user clicked or
    /// dragged to, if any.
    [[nodiscard]] std::optional<std::int64_t>
    draw(const TimelineRange& range, const annotation::AnnotationSession& session,
         std::int64_t current_frame, double fps) const;

    /// One line explaining the colours of the timeline.
    void draw_legend() const;
};

} // namespace ttrally::gui
