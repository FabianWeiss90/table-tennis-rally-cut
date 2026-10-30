// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/timeline_view.hpp"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <vector>

namespace ttrally::gui {

namespace {

constexpr float kHeight = TimelineView::kHeight;
constexpr float kRallyBandTop = 16.0F;
constexpr float kRallyBandBottom = 34.0F;

const ImU32 kBackground = IM_COL32(32, 34, 38, 255);
const ImU32 kHint = IM_COL32(90, 90, 100, 255);
const ImU32 kRally = IM_COL32(56, 132, 230, 200);
const ImU32 kIgnored = IM_COL32(200, 140, 70, 200);
const ImU32 kStart = IM_COL32(70, 200, 90, 255);
const ImU32 kEnd = IM_COL32(230, 80, 70, 255);
const ImU32 kServe = IM_COL32(240, 200, 60, 255);
const ImU32 kCursor = IM_COL32(255, 255, 255, 255);
const ImU32 kTick = IM_COL32(120, 120, 130, 255);
const ImU32 kDragged = IM_COL32(130, 190, 255, 160);
constexpr float kGrabDistance = 6.0F; ///< pixels from an edge within which it can be grabbed

/// An edge on the timeline that can be dragged.
struct Grabbable {
    MarkOwner owner;
    std::int64_t start;
    std::int64_t end;
    annotation::Edge edge;
    float x;
};

/// Coloured square followed by a label, continuing on the same line.
void swatch(ImU32 color, const char* label) {
    const float size = ImGui::GetTextLineHeight() * 0.8F;
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float offset = (ImGui::GetTextLineHeight() - size) / 2;
    ImGui::GetWindowDrawList()->AddRectFilled({position.x, position.y + offset},
                                              {position.x + size, position.y + offset + size},
                                              color);
    ImGui::Dummy({size, ImGui::GetTextLineHeight()});
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 18);
}

} // namespace

void TimelineView::draw_legend() const {
    swatch(kHint, "align hint");
    swatch(kRally, "saved rally");
    swatch(kIgnored, "ignored section");
    swatch(kStart, "start (A)");
    swatch(kEnd, "end (D)");
    swatch(kServe, "serve hit (C)");
    swatch(kCursor, "current frame");
    ImGui::NewLine();
}

TimelineInput TimelineView::draw(const TimelineRange& range,
                                 const annotation::AnnotationSession& session,
                                 std::int64_t current_frame, double fps) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    const auto span = static_cast<double>(std::max<std::int64_t>(1, range.last - range.first));
    auto x_of = [&](std::int64_t frame) {
        const double relative = static_cast<double>(frame - range.first) / span;
        return origin.x + static_cast<float>(relative) * width;
    };
    auto frame_at = [&](float x) {
        const float relative = std::clamp((x - origin.x) / width, 0.0F, 1.0F);
        return range.first + static_cast<std::int64_t>(std::lround(relative * span));
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + width, origin.y + kHeight}, kBackground);

    // One tick per second
    const auto step = static_cast<std::int64_t>(std::max(1.0, std::round(fps)));
    for (std::int64_t f = range.first - range.first % step; f <= range.last; f += step) {
        if (f >= range.first) {
            draw->AddLine({x_of(f), origin.y + kHeight - 6}, {x_of(f), origin.y + kHeight}, kTick);
        }
    }

    // Alignment hint of the current item
    if (const auto item = session.current_item()) {
        const auto& hint = session.plan().item(*item);
        draw->AddRectFilled({x_of(hint.first_frame), origin.y + 2},
                            {x_of(hint.last_frame), origin.y + 8}, kHint);
    }

    // Saved rallies and ignored sections; the one being dragged is shown where it would go
    std::vector<Grabbable> edges;
    auto band = [&](MarkOwner owner, std::int64_t start, std::int64_t end, ImU32 color) {
        if (drag_ && drag_->owner == owner && drag_->original_start == start) {
            (drag_->edge == annotation::Edge::Start ? start : end) = drag_->frame;
            color = kDragged;
        } else if (start <= range.last && range.first <= end) {
            edges.push_back({owner, start, end, annotation::Edge::Start, x_of(start)});
            edges.push_back({owner, start, end, annotation::Edge::End, x_of(end)});
        }
        if (start <= range.last && range.first <= end) {
            draw->AddRectFilled({x_of(std::min(start, end)), origin.y + kRallyBandTop},
                                {x_of(std::max(start, end)), origin.y + kRallyBandBottom},
                                color);
        }
    };
    for (const auto& rally : session.sheet().rallies()) {
        band(MarkOwner::Rally, rally.start_frame, rally.end_frame, kRally);
    }
    for (const auto& section : session.sheet().ignored_sections()) {
        band(MarkOwner::Ignored, section.start_frame, section.end_frame, kIgnored);
    }

    // Draft marks
    const auto& draft = session.draft();
    auto mark = [&](std::optional<std::int64_t> frame, ImU32 color,
                    std::optional<annotation::Edge> edge) {
        if (!frame) {
            return;
        }
        if (edge && drag_ && drag_->owner == MarkOwner::Draft && drag_->edge == *edge) {
            frame = drag_->frame;
        } else if (edge) {
            edges.push_back({MarkOwner::Draft, *frame, *frame, *edge, x_of(*frame)});
        }
        draw->AddLine({x_of(*frame), origin.y}, {x_of(*frame), origin.y + kHeight}, color, 3);
    };
    mark(draft.start_frame, kStart, annotation::Edge::Start);
    mark(draft.end_frame, kEnd, annotation::Edge::End);
    mark(draft.serve_contact_frame, kServe, std::nullopt);
    // Current frame
    draw->AddLine({x_of(current_frame), origin.y}, {x_of(current_frame), origin.y + kHeight},
                  kCursor, 1.5F);

    ImGui::InvisibleButton("##timeline", {width, kHeight});
    const float pointer = ImGui::GetIO().MousePos.x;
    TimelineInput input;
    if (drag_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        drag_->frame = frame_at(pointer);
        input.seek = drag_->frame;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            input.drop = EdgeDrop{drag_->owner, drag_->original_start, drag_->edge, drag_->frame};
            drag_.reset();
        }
        return input;
    }
    // The nearest edge within reach of the pointer can be grabbed (draft marks first: on top)
    const Grabbable* nearest = nullptr;
    for (const Grabbable& edge : edges) {
        const float distance = std::abs(edge.x - pointer);
        if (distance <= kGrabDistance &&
            (nearest == nullptr || distance < std::abs(nearest->x - pointer) ||
             edge.owner == MarkOwner::Draft)) {
            nearest = &edge;
        }
    }
    if (ImGui::IsItemHovered() && nearest != nullptr) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SetTooltip("Drag to move the %s; onto a neighbouring rally to merge them",
                          nearest->edge == annotation::Edge::Start ? "start" : "end");
    }
    if (ImGui::IsItemActivated() && nearest != nullptr) {
        const std::int64_t frame =
            nearest->edge == annotation::Edge::Start ? nearest->start : nearest->end;
        drag_ = Drag{nearest->owner, nearest->start, nearest->end, nearest->edge, frame};
        input.grabbed = nearest->owner;
        input.seek = frame;
        return input;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        input.seek = frame_at(pointer);
    }
    return input;
}

} // namespace ttrally::gui
