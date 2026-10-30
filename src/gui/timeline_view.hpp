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

/// What an edge dragged on the timeline belongs to.
enum class MarkOwner {
    Rally,   ///< a saved rally
    Ignored, ///< a saved ignored section
    Draft,   ///< the start or end mark not saved yet
};

/// An edge dropped at a new frame. Saved entries are named by their start frame before the
/// drag (ids may change while dragging, when the marks are saved).
struct EdgeDrop {
    MarkOwner owner = MarkOwner::Rally;
    std::int64_t original_start = 0;
    annotation::Edge edge = annotation::Edge::Start;
    std::int64_t frame = 0;
};

/// What the user did on the timeline in this frame.
struct TimelineInput {
    std::optional<std::int64_t> seek; ///< frame clicked, or dragged to (also while moving edges)
    std::optional<MarkOwner> grabbed; ///< an edge of this was grabbed
    std::optional<EdgeDrop> drop;     ///< an edge was released
};

/// Timeline of the current review item: the alignment hint, saved rallies and ignored sections,
/// the draft marks and the current frame. Clicking or dragging selects a frame; the edges of
/// rallies, ignored sections and draft marks can be dragged to a new frame.
class TimelineView {
  public:
    static constexpr float kHeight = 56.0F;

    /// Draws the timeline over the full available width.
    [[nodiscard]] TimelineInput draw(const TimelineRange& range,
                                     const annotation::AnnotationSession& session,
                                     std::int64_t current_frame, double fps);

    /// Lets go of the edge being dragged without moving it.
    void cancel_drag() noexcept { drag_.reset(); }

    /// One line explaining the colours of the timeline.
    void draw_legend() const;

  private:
    struct Drag {
        MarkOwner owner;
        std::int64_t original_start;
        std::int64_t original_end;
        annotation::Edge edge;
        std::int64_t frame;
    };
    std::optional<Drag> drag_;
};

} // namespace ttrally::gui
