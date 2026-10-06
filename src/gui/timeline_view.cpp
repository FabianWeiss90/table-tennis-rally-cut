// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/timeline_view.hpp"

#include <algorithm>
#include <cmath>
#include <imgui.h>

namespace ttrally::gui {

namespace {

constexpr float kHeight = TimelineView::kHeight;
constexpr float kRallyBandTop = 16.0F;
constexpr float kRallyBandBottom = 34.0F;

const ImU32 kBackground = IM_COL32(32, 34, 38, 255);
const ImU32 kHint = IM_COL32(90, 90, 100, 255);
const ImU32 kRally = IM_COL32(56, 132, 230, 200);
const ImU32 kStart = IM_COL32(70, 200, 90, 255);
const ImU32 kEnd = IM_COL32(230, 80, 70, 255);
const ImU32 kServe = IM_COL32(240, 200, 60, 255);
const ImU32 kCursor = IM_COL32(255, 255, 255, 255);
const ImU32 kTick = IM_COL32(120, 120, 130, 255);

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
    swatch(kStart, "start (S)");
    swatch(kEnd, "end (E)");
    swatch(kServe, "serve hit (C)");
    swatch(kCursor, "current frame");
    ImGui::NewLine();
}

std::optional<std::int64_t> TimelineView::draw(const TimelineRange& range,
                                               const annotation::AnnotationSession& session,
                                               std::int64_t current_frame, double fps) const {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    const auto span = static_cast<double>(std::max<std::int64_t>(1, range.last - range.first));
    auto x_of = [&](std::int64_t frame) {
        const double relative = static_cast<double>(frame - range.first) / span;
        return origin.x + static_cast<float>(relative) * width;
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
    // Saved rallies
    for (const auto& rally : session.sheet().rallies()) {
        if (rally.overlaps(range.first, range.last)) {
            draw->AddRectFilled({x_of(rally.start_frame), origin.y + kRallyBandTop},
                                {x_of(rally.end_frame), origin.y + kRallyBandBottom}, kRally);
        }
    }
    // Draft marks
    const auto& draft = session.draft();
    auto mark = [&](const std::optional<std::int64_t>& frame, ImU32 color) {
        if (frame) {
            draw->AddLine({x_of(*frame), origin.y}, {x_of(*frame), origin.y + kHeight}, color, 3);
        }
    };
    mark(draft.start_frame, kStart);
    mark(draft.end_frame, kEnd);
    mark(draft.serve_contact_frame, kServe);
    // Current frame
    draw->AddLine({x_of(current_frame), origin.y}, {x_of(current_frame), origin.y + kHeight},
                  kCursor, 1.5F);

    ImGui::InvisibleButton("##timeline", {width, kHeight});
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float pointer = ImGui::GetIO().MousePos.x - origin.x;
        const float relative = std::clamp(pointer / width, 0.0F, 1.0F);
        return range.first + static_cast<std::int64_t>(std::lround(relative * span));
    }
    return std::nullopt;
}

} // namespace ttrally::gui
