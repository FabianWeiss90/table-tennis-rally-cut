// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/annotator_app.hpp"

#include "gui/frame_texture.hpp"
#include "gui/timeline_view.hpp"
#include "shared/io/formatting.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <stdexcept>

namespace ttrally::gui {

namespace {

using annotation::AnnotationRuleViolation;
using annotation::ReviewKind;
using annotation::ReviewStatus;

constexpr float kSidebarWidth = 400.0F;
constexpr int kLegendColumns = 3; ///< Shortcut/action pairs per row
constexpr std::int64_t kFramesBehind = 90;
constexpr std::int64_t kFramesAhead = 90;
constexpr std::int64_t kFramesAheadPlaying = 240;
constexpr double kTimelineMarginSeconds = 2.0;
constexpr std::array kSpeeds{0.1, 0.25, 0.5, 1.0, 2.0, 4.0};
constexpr std::size_t kNormalSpeed = 3;
constexpr float kMaxZoom = 8.0F;

/// Keyboard and mouse controls, always shown below the video.
constexpr std::array<std::pair<const char*, const char*>, 16> kControls{{
    {"Left / Right", "1 frame back / forward"},
    {"Shift + arrows", "10 frames"},
    {"Ctrl + arrows", "1 second"},
    {"Space", "play / pause"},
    {"[  /  ]", "slower / faster"},
    {"Home / End", "start / end of the item"},
    {"S / E", "mark rally start / end"},
    {"C", "mark serve hit (optional)"},
    {"A / L", "toggle aborted toss / let"},
    {"Enter", "save rally, go to next segment"},
    {"Esc", "discard marks"},
    {"X / R", "segment without rally / gap checked"},
    {"N / P", "next / previous open item"},
    {"O", "reopen item"},
    {"Del", "delete the rally at this frame"},
    {"Mouse wheel", "zoom, drag to pan, double-click resets"},
}};

const ImVec4 kErrorColor{1.0F, 0.45F, 0.4F, 1.0F};
const ImVec4 kKeyColor{1.0F, 0.85F, 0.45F, 1.0F};
const ImVec4 kMutedColor{0.6F, 0.6F, 0.65F, 1.0F};
const ImVec4 kDoneColor{0.5F, 0.85F, 0.5F, 1.0F};

ImVec4 status_color(ReviewStatus status) {
    switch (status) {
    case ReviewStatus::Annotated:
        return {0.45F, 0.75F, 1.0F, 1.0F};
    case ReviewStatus::Rejected:
    case ReviewStatus::Reviewed:
        return kMutedColor;
    case ReviewStatus::Open:
        break;
    }
    return {1.0F, 1.0F, 1.0F, 1.0F};
}

/// SDL window, renderer and Dear ImGui context; released in reverse order.
class GuiContext {
  public:
    explicit GuiContext(const std::string& title) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("cannot initialise SDL: ") + SDL_GetError());
        }
        window_ = SDL_CreateWindow(title.c_str(), 1600, 950,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        renderer_ = window_ != nullptr ? SDL_CreateRenderer(window_, nullptr) : nullptr;
        if (renderer_ == nullptr) {
            const std::string error = SDL_GetError();
            release();
            throw std::runtime_error("cannot open the window: " + error);
        }
        SDL_SetRenderVSync(renderer_, 1);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr; // the layout is fixed, nothing to remember
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        ImGui_ImplSDLRenderer3_Init(renderer_);
    }
    ~GuiContext() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        release();
    }
    GuiContext(const GuiContext&) = delete;
    GuiContext& operator=(const GuiContext&) = delete;

    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return renderer_; }

    /// Processes pending events; returns true if the user asked to close the window.
    [[nodiscard]] bool poll_events() const {
        SDL_Event event;
        bool close_requested = false;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                close_requested = true;
            }
        }
        return close_requested;
    }

    void begin_frame() const {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void end_frame(const std::filesystem::path& screenshot = {}) const {
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        if (!screenshot.empty()) {
            save_screenshot(screenshot);
        }
        SDL_RenderPresent(renderer_);
    }

  private:
    void save_screenshot(const std::filesystem::path& path) const {
        SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
        const bool saved = surface != nullptr && SDL_SaveBMP(surface, path.string().c_str());
        SDL_DestroySurface(surface);
        if (!saved) {
            throw std::runtime_error(std::string("cannot save the screenshot: ") + SDL_GetError());
        }
    }

    void release() {
        if (renderer_ != nullptr) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
        SDL_Quit();
    }

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

class AnnotatorApp {
  public:
    AnnotatorApp(annotation::AnnotationSession& session, media::FramePrefetcher& frames,
                 Rational fps, const GuiContext& gui)
        : session_(session), frames_(frames), fps_(fps.value()), texture_(gui.renderer()) {
        if (session_.current_item()) {
            jump_to_item(*session_.current_item());
        }
        request_frames();
    }

    void draw() {
        advance_playback();
        handle_shortcuts();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##annotator", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        const float main_width = ImGui::GetContentRegionAvail().x - kSidebarWidth;
        ImGui::BeginChild("##main", {main_width, 0});
        draw_video();
        draw_transport();
        draw_controls();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##sidebar", {0, 0});
        draw_progress();
        draw_items();
        draw_editor();
        ImGui::EndChild();
        draw_close_dialog();
        ImGui::End();
    }

    /// Closing: saves complete marks, then asks for confirmation if segments have no rally,
    /// gaps were not checked or incomplete marks would be lost.
    void request_close() {
        const bool saved = save_pending_marks();
        if (saved && session_.draft().empty() && session_.plan().progress().complete()) {
            close_confirmed_ = true;
        } else {
            close_dialog_requested_ = true;
        }
    }

    [[nodiscard]] bool close_confirmed() const noexcept { return close_confirmed_; }

    /// Saves marks with start and end before leaving them behind (switching items, closing).
    /// Returns false if they break a rule; the reason is shown and nothing is left.
    bool save_pending_marks() {
        try {
            if (const auto id = session_.save_draft_if_complete()) {
                confirmation_ = std::format("Saved rally {}.", *id);
            }
            return true;
        } catch (const AnnotationRuleViolation& violation) {
            message_ = violation.what();
            return false;
        }
    }

    [[nodiscard]] const std::string& message() const noexcept { return message_; }

  private:
    // ------------------------------------------------------------------ navigation

    [[nodiscard]] std::int64_t last_frame() const { return frames_.frame_count() - 1; }
    [[nodiscard]] std::int64_t one_second() const {
        return static_cast<std::int64_t>(std::lround(fps_));
    }

    void go_to(std::int64_t frame) {
        current_ = std::clamp<std::int64_t>(frame, 0, last_frame());
        request_frames();
    }

    void step(std::int64_t frames) {
        playing_ = false;
        go_to(current_ + frames);
    }

    void request_frames() {
        frames_.request(current_, kFramesBehind, playing_ ? kFramesAheadPlaying : kFramesAhead);
    }

    /// Shows the start of the current item.
    void show_current_item() {
        if (const auto item = session_.current_item()) {
            playing_ = false;
            go_to(session_.plan().item(*item).first_frame);
            scroll_to_current_ = true;
        }
    }

    void jump_to_item(std::size_t index) {
        if (save_pending_marks()) {
            session_.select_item(index);
            show_current_item();
        }
    }

    void select_next_open() {
        if (save_pending_marks() && session_.select_next_open()) {
            show_current_item();
        }
    }

    void select_previous_open() {
        if (save_pending_marks() && session_.select_previous_open()) {
            show_current_item();
        }
    }

    void advance_playback() {
        if (!playing_) {
            return;
        }
        play_clock_ += ImGui::GetIO().DeltaTime * fps_ * kSpeeds[speed_];
        const auto steps = static_cast<std::int64_t>(play_clock_);
        if (steps <= 0) {
            return;
        }
        const std::int64_t target = std::min(current_ + steps, last_frame());
        if (frames_.frame(target)) {
            play_clock_ -= static_cast<double>(steps);
            go_to(target);
        } else {
            play_clock_ = 0.0; // wait for the decoder instead of skipping frames
            request_frames();
        }
        if (current_ >= last_frame()) {
            playing_ = false;
        }
    }

    void toggle_playback() {
        playing_ = !playing_;
        play_clock_ = 0.0;
        request_frames();
    }

    // ------------------------------------------------------------------ editing

    /// Runs a session command and shows a rule violation instead of throwing.
    template <typename Command> void apply(Command&& command) {
        try {
            command();
            message_.clear();
        } catch (const AnnotationRuleViolation& violation) {
            message_ = violation.what();
        }
    }

    /// R: marks the gap as checked and continues with the next gap that is not done yet.
    void mark_gap_checked() {
        apply([this] {
            session_.mark_current_reviewed();
            if (session_.select_next_open_gap()) {
                show_current_item();
            }
        });
    }

    /// Enter: saves the rally and continues with the next segment that has no rally yet.
    void save_draft() {
        apply([this] {
            const int id = session_.save_draft();
            confirmation_ = std::format("Saved rally {}.", id);
            if (session_.select_next_open_candidate()) {
                show_current_item();
            }
        });
    }


    void handle_shortcuts() {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            return; // typing notes or answering a dialog
        }
        const bool shift = io.KeyShift;
        const bool ctrl = io.KeyCtrl;
        const std::int64_t stride = ctrl ? one_second() : (shift ? 10 : 1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            step(-stride);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            step(stride);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            toggle_playback();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) && speed_ > 0) {
            --speed_;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false) && speed_ + 1 < kSpeeds.size()) {
            ++speed_;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            session_.mark_start(current_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            session_.mark_end(current_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            session_.mark_serve_contact(current_);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            session_.set_aborted_toss(!session_.draft().aborted_toss);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
            session_.set_let(!session_.draft().let);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            save_draft();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            session_.discard_draft();
            message_.clear();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (const auto id = session_.delete_rally_at(current_)) {
                confirmation_ = std::format("Deleted rally {}.", *id);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            apply([this] { session_.reject_current(); });
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            mark_gap_checked();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            apply([this] { session_.reopen_current(); });
        }
        if (ImGui::IsKeyPressed(ImGuiKey_N, false) ||
            ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
            select_next_open();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_P, false) ||
            ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
            select_previous_open();
        }
        if (const auto item = session_.current_item()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                step(session_.plan().item(*item).first_frame - current_);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                step(session_.plan().item(*item).last_frame - current_);
            }
        }
    }

    // ------------------------------------------------------------------ drawing

    void draw_video() {
        const float height = ImGui::GetContentRegionAvail().y - footer_height();
        ImGui::BeginChild("##video", {0, height}, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (const auto frame = frames_.frame(current_)) {
            texture_.show(*frame);
        }
        if (const auto error = frames_.error()) {
            ImGui::TextColored(kErrorColor, "Decoding failed: %s", error->c_str());
        } else if (texture_.texture() != nullptr) {
            draw_image();
        } else {
            ImGui::TextUnformatted("Decoding...");
        }
        ImGui::EndChild();
    }

    void draw_image() {
        const ImVec2 area = ImGui::GetContentRegionAvail();
        const auto size = texture_.size();
        const float scale = std::min(area.x / static_cast<float>(size.width),
                                     area.y / static_cast<float>(size.height));
        const ImVec2 shown{static_cast<float>(size.width) * scale,
                           static_cast<float>(size.height) * scale};
        ImGui::SetCursorPos({(area.x - shown.x) / 2, (area.y - shown.y) / 2});
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // Zoom with the mouse wheel around the pointer, pan by dragging, reset by double-click.
        const float view = 1.0F / zoom_;
        const ImVec2 uv0{centre_.x - view / 2, centre_.y - view / 2};
        const ImVec2 uv1{centre_.x + view / 2, centre_.y + view / 2};
        ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(texture_.texture())), shown, uv0,
                     uv1);
        if (ImGui::IsItemHovered()) {
            handle_zoom(origin, shown, uv0, view);
        }

        const std::string label =
            std::format("Frame {} / {}   {}{}", current_, last_frame(),
                        io::format_clock(static_cast<double>(current_) / fps_),
                        texture_.frame_index() != current_ ? "   (decoding...)" : "");
        const float line = ImGui::GetTextLineHeightWithSpacing() + 4;
        ImVec2 position{origin.x + 8, origin.y + 8};
        draw_overlay_text(position, label, IM_COL32(255, 255, 255, 255));
        if (const auto rally = session_.sheet().rally_at(current_)) {
            position.y += line;
            draw_overlay_text(position, std::format("in rally {}", *rally),
                              IM_COL32(110, 180, 255, 255));
        }
        if (const auto hint = unsaved_hint()) {
            position.y += line;
            draw_overlay_text(position, *hint, IM_COL32(255, 120, 100, 255));
        }
    }

    /// What is missing to save the current marks, if there are any.
    [[nodiscard]] std::optional<std::string> unsaved_hint() const {
        const auto& draft = session_.draft();
        if (draft.empty()) {
            return std::nullopt;
        }
        if (!draft.start_frame) {
            return "Not saved: mark the start (S)";
        }
        if (!draft.end_frame) {
            return "Not saved: mark the end (E)";
        }
        return "Not saved: press Enter";
    }

    /// Text on a dark box, readable on any video content.
    static void draw_overlay_text(const ImVec2& position, const std::string& text, ImU32 color) {
        constexpr float kPadding = 4.0F;
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled({position.x - kPadding, position.y - kPadding},
                            {position.x + size.x + kPadding, position.y + size.y + kPadding},
                            IM_COL32(0, 0, 0, 170), 3.0F);
        draw->AddText(position, color, text.c_str());
    }

    void handle_zoom(const ImVec2& origin, const ImVec2& shown, const ImVec2& uv0, float view) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 pointer{uv0.x + (io.MousePos.x - origin.x) / shown.x * view,
                             uv0.y + (io.MousePos.y - origin.y) / shown.y * view};
        if (io.MouseWheel != 0.0F) {
            const float old_zoom = zoom_;
            zoom_ = std::clamp(zoom_ * std::pow(1.25F, io.MouseWheel), 1.0F, kMaxZoom);
            // Keep the point under the pointer in place.
            centre_.x = pointer.x + (centre_.x - pointer.x) * old_zoom / zoom_;
            centre_.y = pointer.y + (centre_.y - pointer.y) * old_zoom / zoom_;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            centre_.x -= io.MouseDelta.x / shown.x * view;
            centre_.y -= io.MouseDelta.y / shown.y * view;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            zoom_ = 1.0F;
            centre_ = {0.5F, 0.5F};
        }
        const float half = 0.5F / zoom_;
        centre_.x = std::clamp(centre_.x, half, 1.0F - half);
        centre_.y = std::clamp(centre_.y, half, 1.0F - half);
    }

    /// Height of everything below the video: timeline, its legend, buttons and controls.
    [[nodiscard]] static float footer_height() {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float line = ImGui::GetTextLineHeightWithSpacing();
        const auto control_rows = static_cast<float>((kControls.size() + kLegendColumns - 1) /
                                                     kLegendColumns);
        return TimelineView::kHeight + style.ItemSpacing.y + line +
               ImGui::GetFrameHeightWithSpacing() + 2 * style.ItemSpacing.y +
               control_rows * (line + style.CellPadding.y * 2) + style.WindowPadding.y;
    }

    void draw_transport() {
        const auto margin = static_cast<std::int64_t>(kTimelineMarginSeconds * fps_);
        TimelineRange range{std::max<std::int64_t>(0, current_ - 5 * one_second()),
                            std::min(last_frame(), current_ + 5 * one_second())};
        if (const auto item = session_.current_item()) {
            const auto& hint = session_.plan().item(*item);
            range = {std::max<std::int64_t>(0, std::min(hint.first_frame, current_) - margin),
                     std::min(last_frame(), std::max(hint.last_frame, current_) + margin)};
        }
        if (const auto clicked = timeline_.draw(range, session_, current_, fps_)) {
            playing_ = false;
            go_to(*clicked);
        }
        timeline_.draw_legend();

        if (ImGui::Button("<< 1 s")) {
            step(-one_second());
        }
        ImGui::SameLine();
        if (ImGui::Button("< 1")) {
            step(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button(playing_ ? "Pause" : "Play", {70, 0})) {
            toggle_playback();
        }
        ImGui::SameLine();
        if (ImGui::Button("1 >")) {
            step(1);
        }
        ImGui::SameLine();
        if (ImGui::Button("1 s >>")) {
            step(one_second());
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("Speed", std::format("{}x", kSpeeds[speed_]).c_str())) {
            for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
                if (ImGui::Selectable(std::format("{}x", kSpeeds[i]).c_str(), i == speed_)) {
                    speed_ = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(kMutedColor, "Decoder: %s",
                           std::string(media::to_string(frames_.backend())).c_str());
    }

    void draw_progress() const {
        const auto progress = session_.plan().progress();
        ImGui::Text("Video: %s", session_.sheet().video_id().c_str());
        const std::size_t lets = session_.sheet().let_count();
        ImGui::Text("Segments %zu/%zu   Gaps %zu/%zu   Rallies %zu (incl. %zu %s)",
                    progress.candidates_done, progress.candidates, progress.gaps_done,
                    progress.gaps, session_.sheet().rallies().size(), lets,
                    lets == 1 ? "let" : "lets");
        if (progress.complete()) {
            ImGui::TextColored(kDoneColor, "Complete: the video can be used for training.");
        } else {
            ImGui::TextColored(kMutedColor,
                               "Training uses the video once every segment and gap is done.");
        }
        ImGui::Separator();
    }

    void draw_items() {
        ImGui::BeginChild("##items", {0, ImGui::GetContentRegionAvail().y * 0.4F},
                          ImGuiChildFlags_Borders);
        if (ImGui::BeginTable("##item_table", 3, ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Item");
            ImGui::TableSetupColumn("Frames");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();
            const auto& items = session_.plan().items();
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto& item = items[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, status_color(item.status));
                const bool selected = session_.current_item() == i;
                if (ImGui::Selectable(std::format("{}##{}", item.title(), i).c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    jump_to_item(i);
                }
                if (selected && scroll_to_current_) {
                    ImGui::SetScrollHereY();
                }
                ImGui::TableNextColumn();
                ImGui::Text("%lld-%lld", static_cast<long long>(item.first_frame),
                            static_cast<long long>(item.last_frame));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(annotation::to_string(item.status)).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        scroll_to_current_ = false;
        ImGui::EndChild();
    }

    void draw_frame_field(const char* label, const std::optional<std::int64_t>& frame,
                          ImGuiKey key) {
        ImGui::Text("%-14s %s", label,
                    frame ? std::to_string(*frame).c_str() : "-");
        ImGui::SameLine(220);
        if (ImGui::SmallButton(std::format("Set##{}", label).c_str())) {
            if (key == ImGuiKey_S) {
                session_.mark_start(current_);
            } else if (key == ImGuiKey_E) {
                session_.mark_end(current_);
            } else {
                session_.mark_serve_contact(current_);
            }
        }
        if (frame) {
            ImGui::SameLine();
            if (ImGui::SmallButton(std::format("Go##{}", label).c_str())) {
                step(*frame - current_);
            }
        }
    }

    void draw_editor() {
        ImGui::Separator();
        const auto& draft = session_.draft();
        const std::string heading =
            draft.editing ? std::format("Editing rally {}", *draft.editing) : "New rally";
        ImGui::TextUnformatted(heading.c_str());
        draw_frame_field("Start (S)", draft.start_frame, ImGuiKey_S);
        draw_frame_field("End (E)", draft.end_frame, ImGuiKey_E);
        draw_frame_field("Serve hit (C)", draft.serve_contact_frame, ImGuiKey_C);
        if (draft.serve_contact_frame) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##serve")) {
                session_.clear_serve_contact();
            }
        }
        if (draft.complete()) {
            const std::int64_t length = *draft.end_frame - *draft.start_frame + 1;
            ImGui::TextColored(kMutedColor, "Length %lld frames (%.2f s)",
                               static_cast<long long>(length), static_cast<double>(length) / fps_);
        }
        bool aborted = draft.aborted_toss;
        if (ImGui::Checkbox("Aborted toss (A)", &aborted)) {
            session_.set_aborted_toss(aborted);
        }
        ImGui::SameLine();
        bool let = draft.let;
        if (ImGui::Checkbox("Let (L)", &let)) {
            session_.set_let(let);
        }
        draw_notes_field();

        if (ImGui::Button("Save rally (Enter)")) {
            save_draft();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard (Esc)")) {
            session_.discard_draft();
            message_.clear();
        }
        draw_item_actions();
        if (!message_.empty()) {
            ImGui::TextColored(kErrorColor, "%s", message_.c_str());
        } else if (!confirmation_.empty()) {
            ImGui::TextColored(kMutedColor, "%s", confirmation_.c_str());
        }
        draw_rallies_of_item();
    }

    void draw_notes_field() {
        const auto& notes = session_.draft().notes;
        if (notes != std::string(notes_buffer_.data())) {
            const std::size_t length = std::min(notes.size(), notes_buffer_.size() - 1);
            std::copy_n(notes.begin(), length, notes_buffer_.begin());
            notes_buffer_[length] = '\0';
        }
        if (ImGui::InputText("Notes", notes_buffer_.data(), notes_buffer_.size())) {
            session_.set_notes(notes_buffer_.data());
        }
    }

    void draw_item_actions() {
        const auto index = session_.current_item();
        if (!index) {
            return;
        }
        const auto& item = session_.plan().item(*index);
        if (item.kind == ReviewKind::Candidate) {
            if (ImGui::Button("No rally here (X)")) {
                apply([this] { session_.reject_current(); });
            }
        } else if (ImGui::Button("Gap checked (R)")) {
            mark_gap_checked();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reopen (O)")) {
            apply([this] { session_.reopen_current(); });
        }
        ImGui::SameLine();
        if (ImGui::Button("Next open (N)")) {
            select_next_open();
        }
    }

    void draw_rallies_of_item() {
        const auto index = session_.current_item();
        if (!index) {
            return;
        }
        const auto& item = session_.plan().item(*index);
        const auto ids = session_.sheet().rallies_overlapping(item.first_frame, item.last_frame);
        ImGui::Separator();
        ImGui::Text("Rallies in %s: %zu", item.title().c_str(), ids.size());
        for (const int id : ids) {
            const auto& rally = session_.sheet().rally(id);
            ImGui::Text("%d: %lld-%lld%s", id, static_cast<long long>(rally.start_frame),
                        static_cast<long long>(rally.end_frame),
                        rally.aborted_toss ? " (aborted toss)" : (rally.let ? " (let)" : ""));
            ImGui::SameLine();
            if (ImGui::SmallButton(std::format("Go##rally{}", id).c_str())) {
                step(rally.start_frame - current_);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(std::format("Edit##rally{}", id).c_str())) {
                session_.edit_rally(id);
                step(rally.start_frame - current_);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(std::format("Delete##rally{}", id).c_str())) {
                session_.delete_rally(id);
                confirmation_ = std::format("Deleted rally {}.", id);
                break; // ids changed
            }
        }
    }

    void draw_close_dialog() {
        constexpr const char* kTitle = "Close the annotation?";
        if (close_dialog_requested_) {
            ImGui::OpenPopup(kTitle);
            close_dialog_requested_ = false;
        }
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }
        if (!message_.empty()) {
            ImGui::TextColored(kErrorColor, "The marked rally cannot be saved: %s",
                               message_.c_str());
        } else if (!session_.draft().empty()) {
            ImGui::TextColored(kErrorColor, "The current marks are incomplete and will be lost.");
        }
        const auto segments = session_.segments_without_rally();
        if (!segments.empty()) {
            ImGui::Text("%zu segment(s) have no rally:", segments.size());
            ImGui::TextWrapped("%s", segment_list(segments).c_str());
            ImGui::TextColored(kMutedColor, "Press X on a segment that really has no rally.");
        }
        const auto gaps = session_.unchecked_gaps();
        if (!gaps.empty()) {
            ImGui::Text("%zu gap(s) were not checked:", gaps.size());
            ImGui::TextWrapped("%s", segment_list(gaps).c_str());
            ImGui::TextColored(kMutedColor, "Watch each gap and press R if it has no rally.");
        }
        if (!segments.empty() || !gaps.empty()) {
            ImGui::TextColored(kErrorColor,
                               "Training skips this video until every item is done.");
        }
        const auto first_open = session_.plan().first_open();
        ImGui::Spacing();
        if (ImGui::Button("Close anyway")) {
            close_confirmed_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
            if (first_open) {
                session_.select_item(*first_open);
                show_current_item();
            }
        }
        ImGui::EndPopup();
    }

    /// "Segment 4, Segment 17, ..." (at most 20 names).
    [[nodiscard]] std::string segment_list(const std::vector<std::size_t>& items) const {
        constexpr std::size_t kMaxNames = 20;
        std::string text;
        for (std::size_t i = 0; i < std::min(items.size(), kMaxNames); ++i) {
            text += (i > 0 ? ", " : "") + session_.plan().item(items[i]).title();
        }
        if (items.size() > kMaxNames) {
            text += std::format(" and {} more", items.size() - kMaxNames);
        }
        return text;
    }

    static void draw_controls() {
        ImGui::Separator();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {10.0F, 2.0F});
        const bool table = ImGui::BeginTable("##controls", 2 * kLegendColumns,
                                             ImGuiTableFlags_SizingFixedFit);
        ImGui::PopStyleVar();
        if (!table) {
            return;
        }
        for (const auto& [keys, action] : kControls) {
            ImGui::TableNextColumn();
            ImGui::TextColored(kKeyColor, "%s", keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(action);
        }
        ImGui::EndTable();
    }

    annotation::AnnotationSession& session_;
    media::FramePrefetcher& frames_;
    double fps_;
    FrameTexture texture_;
    TimelineView timeline_;
    std::int64_t current_ = 0;
    bool playing_ = false;
    double play_clock_ = 0.0;
    std::size_t speed_ = kNormalSpeed;
    float zoom_ = 1.0F;
    ImVec2 centre_{0.5F, 0.5F};
    std::array<char, 512> notes_buffer_{};
    std::string message_;
    std::string confirmation_;
    bool scroll_to_current_ = true;
    bool close_dialog_requested_ = false;
    bool close_confirmed_ = false;
};

} // namespace

void run_annotator(annotation::AnnotationSession& session, media::FramePrefetcher& frames,
                   Rational fps, const AnnotatorOptions& options) {
    const GuiContext gui(options.title);
    AnnotatorApp app(session, frames, fps, gui);
    for (int rendered = 0; options.max_frames == 0 || rendered < options.max_frames; ++rendered) {
        if (gui.poll_events()) {
            app.request_close();
        }
        if (app.close_confirmed()) {
            return;
        }
        gui.begin_frame();
        app.draw();
        const bool last = options.max_frames != 0 && rendered + 1 == options.max_frames;
        gui.end_frame(last ? options.screenshot : std::filesystem::path{});
    }
    if (!app.save_pending_marks()) {
        throw std::runtime_error("The last marks were not saved: " + app.message());
    }
}

} // namespace ttrally::gui
