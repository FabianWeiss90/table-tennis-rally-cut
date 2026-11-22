// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/launcher_app.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "gui/highlights_panel.hpp"
#include "launcher/domain/batch.hpp"
#include "launcher/domain/scenes.hpp"
#include "shared/io/formatting.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <future>
#include <imgui.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ttrally::gui {

namespace {

using launcher::AnnotationProgress;
using launcher::CommandForm;
using launcher::Field;
using launcher::FieldKind;
using launcher::VideoEntry;

constexpr float kMinSidebarWidth = 380.0F; ///< initially wider if the status line needs it
constexpr float kNarrowestSidebar = 240.0F; ///< the user cannot drag it narrower
constexpr float kLibraryButtonsWidth = 140.0F;
/// The steps of draw_status(), for measuring its line
constexpr std::array kSteps{"align", "labels", "features", "detect", "refine", "cut"};
constexpr float kLabelWidth = 230.0F;
constexpr std::size_t kNumberFieldDigits = 8; ///< width of a number field, e.g. "3072" or "1.5"
constexpr float kLogShare = 0.38F; ///< of the right side's height, at first (dragged later)
constexpr float kSplitterHeight = 8.0F;  ///< the bar between the tabs and the output
constexpr float kMinPaneHeight = 80.0F;  ///< of the tabs and of the output when dragged
constexpr float kMaxLogShare = 0.5F;     ///< limit of kMinPaneHeight's share in a tiny window
constexpr int kFitFrames = 4; ///< frames to fit the output to the highlights player (layout)
constexpr int kPreviewHeight = 360;  ///< decoded preview size (shown scaled to the sidebar)
constexpr int kDefaultPreviewPercent = 20; ///< far enough in to be past the warm-up
constexpr float kSelectedVideoHeight = 440.0F; ///< preview and details below the video list
constexpr int kThumbnailHeight = 72;           ///< decoded and shown size of the thumbnails
constexpr float kThumbnailWidth = 128.0F;
constexpr float kSceneComboWidth = 64.0F;
constexpr const char* kNoScene = "-";
const ImVec4 kDone{0.5F, 0.85F, 0.5F, 1.0F};
const ImVec4 kPartly{0.95F, 0.75F, 0.35F, 1.0F};
const ImVec4 kMuted{0.6F, 0.6F, 0.65F, 1.0F};
const ImVec4 kError{1.0F, 0.45F, 0.4F, 1.0F};
constexpr const char* kHighlightsForm = "highlights";
constexpr const char* kVideoIdFlag = "--video-id";
constexpr const char* kMatchDayFlag = "--match-day";
constexpr const char* kPipelineTab = "pipeline"; ///< for LauncherOptions::first_tab
constexpr const char* kMergeTab = "merge";       ///< for LauncherOptions::first_tab
constexpr const char* kTrainForm = "train"; ///< the same for a video and a whole match day
constexpr double kDefaultCrossfade = 0.5;   ///< of `ttrally highlights`

/// Text input on a std::string (Dear ImGui works on char buffers); the hint shows while empty.
bool input_text(const char* id, std::string& value, ImGuiInputTextFlags flags = 0,
                const char* hint = "") {
    auto resize = [](ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* text = static_cast<std::string*>(data->UserData);
            text->resize(static_cast<std::size_t>(data->BufTextLen));
            data->Buf = text->data();
        }
        return 0;
    };
    return ImGui::InputTextWithHint(id, hint, value.data(), value.capacity() + 1,
                                    flags | ImGuiInputTextFlags_CallbackResize, resize, &value);
}

std::vector<std::string> lines_of(const std::string& value) {
    std::vector<std::string> lines;
    std::istringstream in(value);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

bool is_output(const Field& field) { return field.flag == "--out" || field.flag == "--report"; }

/// Splits a video key "<match day>/<video id>".
std::pair<std::string, std::string> split_key(const std::string& key) {
    const auto slash = key.rfind('/');
    return slash == std::string::npos
               ? std::pair{std::string(), key}
               : std::pair{key.substr(0, slash), key.substr(slash + 1)};
}

/// Decodes preview frames on a background thread, so the window stays responsive; only the
/// latest request counts.
class PreviewWorker {
  public:
    explicit PreviewWorker(launcher::VideoPreview& preview)
        : preview_(preview), thread_([this] { loop(); }) {}
    ~PreviewWorker() {
        {
            const std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    PreviewWorker(const PreviewWorker&) = delete;
    PreviewWorker& operator=(const PreviewWorker&) = delete;

    void request(std::filesystem::path video, double fraction) {
        {
            const std::lock_guard lock(mutex_);
            pending_ = Request{std::move(video), fraction};
            busy_ = true;
        }
        wake_.notify_one();
    }

    /// A finished preview: the frame, or nullopt inside if the video could not be read.
    [[nodiscard]] std::optional<std::optional<media::VideoFrame>> take_result() {
        const std::lock_guard lock(mutex_);
        return std::exchange(result_, std::nullopt);
    }

    [[nodiscard]] bool busy() const {
        const std::lock_guard lock(mutex_);
        return busy_;
    }

  private:
    struct Request {
        std::filesystem::path video;
        double fraction = 0.0;
    };

    void loop() {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || pending_; });
                if (stop_) {
                    return;
                }
                request = std::move(*pending_);
                pending_.reset();
            }
            auto frame = preview_.frame_at(request.video, request.fraction, kPreviewHeight);
            const std::lock_guard lock(mutex_);
            if (!pending_) { // otherwise a newer request makes this one obsolete
                result_ = std::move(frame);
                busy_ = false;
            }
        }
    }

    launcher::VideoPreview& preview_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::optional<std::optional<media::VideoFrame>> result_;
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_; ///< last member: started after everything it uses
};

/// Decodes thumbnails (one frame per video) one after another on a background thread.
class ThumbnailWorker {
  public:
    explicit ThumbnailWorker(launcher::VideoPreview& preview)
        : preview_(preview), thread_([this] { loop(); }) {}
    ~ThumbnailWorker() {
        {
            const std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    ThumbnailWorker(const ThumbnailWorker&) = delete;
    ThumbnailWorker& operator=(const ThumbnailWorker&) = delete;

    void request(std::string video) {
        {
            const std::lock_guard lock(mutex_);
            pending_.push_back(std::move(video));
        }
        wake_.notify_one();
    }

    /// Finished thumbnails: video and frame (nullopt if the video could not be read).
    [[nodiscard]] std::vector<std::pair<std::string, std::optional<media::VideoFrame>>>
    take_results() {
        const std::lock_guard lock(mutex_);
        return std::exchange(results_, {});
    }

  private:
    void loop() {
        for (;;) {
            std::string video;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || !pending_.empty(); });
                if (stop_) {
                    return;
                }
                video = std::move(pending_.front());
                pending_.pop_front();
            }
            auto frame = preview_.frame_at(video, kDefaultPreviewPercent / 100.0, kThumbnailHeight);
            const std::lock_guard lock(mutex_);
            results_.emplace_back(std::move(video), std::move(frame));
        }
    }

    launcher::VideoPreview& preview_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> pending_;
    std::vector<std::pair<std::string, std::optional<media::VideoFrame>>> results_;
    bool stop_ = false;
    std::thread thread_; ///< last member: started after everything it uses
};

/// Dialog target meaning "the library folder" instead of a form field.
constexpr std::size_t kLibraryTarget = static_cast<std::size_t>(-1);

/// A path chosen in a native file dialog, delivered on SDL's thread and applied on ours.
struct DialogResult {
    std::size_t form = 0;
    std::size_t field = 0;
    std::string path;
};

class LauncherApp {
  public:
    LauncherApp(launcher::LauncherSession& session, launcher::VideoPreview& preview,
                const GuiContext& gui, const LauncherOptions& options)
        : session_(session), preview_(preview), gui_(gui),
          project_(std::filesystem::absolute(options.project_directory)),
          settings_(options.settings), scenes_(options.scenes), first_tab_(options.first_tab),
          preview_worker_(preview), preview_texture_(gui.renderer()), thumbnail_worker_(preview) {
        if (options.highlights != nullptr && options.playback != nullptr &&
            options.fonts != nullptr) {
            highlights_ = std::make_unique<HighlightsPanel>(gui, *options.highlights,
                                                            *options.playback, *options.fonts);
        }
        restore_last_video();
    }

    void draw() {
        session_.poll();
        apply_dialog_result();
        collect_thumbnails();
        collect_scenes();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        // Resizable at its right border; the given width is only the initial one
        ImGui::SetNextWindowSizeConstraints({kNarrowestSidebar, 0}, {FLT_MAX, FLT_MAX});
        ImGui::BeginChild("##videos", {initial_sidebar_width(), 0},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        draw_videos();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##commands", {0, 0});
        const float height = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##forms", {0, std::floor(height * (1.0F - log_share_))});
        draw_forms();
        ImGui::EndChild();
        fit_output_to_player(height);
        draw_splitter(height);
        draw_log();
        ImGui::EndChild();
        ImGui::End();
    }

  private:
    /// The bar between the tabs and the output: dragged up or down, it shares the height
    /// between them (kept as a share, so both grow with the window).
    void draw_splitter(float height) {
        ImGui::InvisibleButton("##splitter", {-FLT_MIN, kSplitterHeight});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (ImGui::IsItemActive() && height > 0) {
            set_log_share(log_share_ - ImGui::GetIO().MouseDelta.y / height, height);
            fit_frames_ = 0; // the user's choice from now on
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float middle = (min.y + max.y) / 2;
        ImGui::GetWindowDrawList()->AddLine(
            {min.x, middle}, {max.x, middle},
            ImGui::GetColorU32(ImGui::IsItemActive() || ImGui::IsItemHovered()
                                   ? ImGuiCol_SeparatorActive
                                   : ImGuiCol_Separator),
            2.0F);
        ImGui::SetItemTooltip("Drag to share the height between the tabs and the output");
    }

    /// After the highlights were opened: moves the bar above the output so that the rally list
    /// is exactly as high as the player (a few frames, until the layout has settled).
    void fit_output_to_player(float height) {
        if (fit_frames_ == 0 || !highlights_ || !highlights_shown_ || height <= 0) {
            return;
        }
        --fit_frames_;
        set_log_share(log_share_ + highlights_->spare_height() / height, height);
    }

    void set_log_share(float share, float height) {
        const float lowest = std::min(kMinPaneHeight / height, kMaxLogShare);
        log_share_ = std::clamp(share, lowest, 1.0F - lowest);
    }

    // ------------------------------------------------------------------ videos

    /// Wide enough for a video's whole status line (every step), next to the list's scrollbar.
    static float initial_sidebar_width() {
        const ImGuiStyle& style = ImGui::GetStyle();
        float width = style.IndentSpacing + 2 * style.WindowPadding.x + style.ScrollbarSize;
        for (const char* name : kSteps) {
            width += ImGui::CalcTextSize(std::format("+ {}", name).c_str()).x + style.ItemSpacing.x;
        }
        return std::max(kMinSidebarWidth, width);
    }

    /// A match day's collapsible header (closed at first; `open`: opened now); true if open.
    bool draw_match_day_folder(const std::string& match_day, bool open) {
        const auto& videos = session_.workspace().videos;
        const auto count = std::ranges::count(videos, match_day, &VideoEntry::match_day);
        ImGui::SetNextItemOpen(open, open ? ImGuiCond_Always : ImGuiCond_Once);
        const bool is_open = ImGui::CollapsingHeader(
            std::format("{}   ({} video{})###day:{}", match_day, count, count == 1 ? "" : "s",
                        match_day)
                .c_str());
        if (is_open) {
            draw_scene_detection(match_day);
            draw_match_day_entry(match_day, count);
        }
        return is_open;
    }

    /// The match day as a whole, above its videos: batches, pipeline and its highlights.
    void draw_match_day_entry(const std::string& match_day, std::ptrdiff_t videos) {
        ImGui::PushID(match_day.c_str());
        const bool selected = session_.selected_match_day() == match_day;
        const std::string label =
            std::format("Whole match day ({} game{})", videos, videos == 1 ? "" : "s");
        if (ImGui::Selectable(label.c_str(), selected) &&
            !session_.running()) {
            session_.select_match_day(match_day);
        }
        ImGui::SetItemTooltip("Run steps for all games at once (pipeline) and choose the match "
                              "day highlights from all its rallies");
        ImGui::PopID();
        ImGui::Spacing();
    }

    /// Selects the video chosen in the last session, so that its match day is the open one.
    void restore_last_video() {
        const auto last = settings_ != nullptr ? settings_->last_video() : std::nullopt;
        if (!last) {
            return;
        }
        const auto& videos = session_.workspace().videos;
        for (std::size_t i = 0; i < videos.size(); ++i) {
            if (std::filesystem::path(videos[i].original).lexically_normal() ==
                last->lexically_normal()) {
                session_.select_video(i);
                return;
            }
        }
    }

    /// Saves a newly selected video as the last one; returns its match day if the selection
    /// changed (that folder is opened), else an empty string.
    std::string remember_selection() {
        const auto selected = session_.selected_video();
        if (selected == remembered_) {
            return {};
        }
        remembered_ = selected;
        if (!selected) {
            return {};
        }
        const VideoEntry& video = session_.workspace().videos[*selected];
        if (settings_ != nullptr) {
            settings_->save_last_video(video.original);
        }
        return video.match_day;
    }

    void draw_library() {
        ImGui::TextUnformatted("Library");
        ImGui::SameLine(ImGui::GetWindowWidth() - kLibraryButtonsWidth);
        ImGui::BeginDisabled(session_.running());
        if (ImGui::SmallButton("Choose...")) {
            pending_target_ = {kLibraryTarget, 0, {}};
            const std::string start = session_.library().string();
            SDL_ShowOpenFolderDialog(&LauncherApp::on_dialog, this, gui_.window(),
                                     start.empty() ? nullptr : start.c_str(), false);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Folder with one folder per match day");
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan")) {
            session_.rescan();
        }
        const std::string& library = session_.workspace().library;
        ImGui::TextColored(library.empty() ? kError : kMuted, "%s",
                           library.empty() ? "none chosen" : library.c_str());
        if (!library.empty()) {
            ImGui::SetItemTooltip("%s", library.c_str());
        }
        ImGui::Separator();
    }

    void draw_videos() {
        draw_library();
        const auto& videos = session_.workspace().videos;
        if (videos.empty()) {
            ImGui::TextWrapped(
                "No videos found. Choose the library: a folder with one folder per match day, "
                "each holding the original videos and a folder with their Liimba cuts. The "
                "tool's files go into a folder ttrally-data in every match day.");
        }
        const bool selected_any = session_.selected_video().has_value();
        const std::string open_day = remember_selection();
        ImGui::BeginChild("##list", {0, selected_any ? -kSelectedVideoHeight : 0});
        bool day_open = true;
        for (std::size_t i = 0; i < videos.size(); ++i) {
            const VideoEntry& video = videos[i];
            if (i == 0 || video.match_day != videos[i - 1].match_day) {
                day_open = draw_match_day_folder(video.match_day, video.match_day == open_day);
            }
            if (!day_open) {
                continue;
            }
            ImGui::PushID(static_cast<int>(i));
            const std::string label =
                video.scene.empty() ? video.video_id : video.video_id + "   (scene " + video.scene + ")";
            if (ImGui::Selectable(label.c_str(), !session_.selected_match_day() &&
                                                     session_.selected_video() == i)) {
                if (!session_.running()) {
                    session_.select_video(i);
                }
            }
            draw_status(video);
            ImGui::PopID();
            ImGui::Spacing();
        }
        ImGui::EndChild();
        if (const auto selected = session_.selected_video()) {
            ImGui::Separator();
            // Its own scrolling area with a scrollbar always shown: otherwise long details make
            // the sidebar scroll, which narrows the preview, which removes the scrollbar again
            // (flickering every frame)
            ImGui::BeginChild("##selected", {0, 0}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (const auto& day = session_.selected_match_day()) {
                draw_match_day_details(*day);
            } else {
                draw_preview(videos[*selected]);
                draw_details(videos[*selected]);
            }
            ImGui::EndChild();
        }
    }

    void draw_preview(const VideoEntry& video) {
        if (video.original != preview_video_) {
            preview_video_ = video.original;
            preview_shown_ = false;
            preview_worker_.request(project_ / video.original, preview_percent_ / 100.0);
        }
        if (auto result = preview_worker_.take_result()) {
            preview_failed_ = !result->has_value();
            if (result->has_value()) {
                media::VideoFrame& frame = **result;
                frame.index = ++preview_count_; // a new frame for the texture each time
                preview_time_ = frame.time_s;
                preview_texture_.show(frame);
                preview_shown_ = true;
            }
        }
        const float width = ImGui::GetContentRegionAvail().x;
        if (preview_shown_ && preview_texture_.texture() != nullptr) {
            const auto size = preview_texture_.size();
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(preview_texture_.texture())),
                         {width, width * static_cast<float>(size.height) /
                                     static_cast<float>(size.width)});
        } else {
            ImGui::TextColored(preview_failed_ ? kError : kMuted, "%s",
                               preview_failed_ ? "No preview (the video cannot be read)"
                                               : "Loading the preview...");
        }
        ImGui::SetNextItemWidth(width - 110);
        ImGui::SliderInt("##position", &preview_percent_, 0, 100, "%d %%");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            preview_worker_.request(project_ / video.original, preview_percent_ / 100.0);
        }
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s",
                           preview_worker_.busy() ? "..."
                                                  : io::format_clock(preview_time_).c_str());
        ImGui::Spacing();
    }

    static void step(const char* name, bool done, bool partly = false) {
        ImGui::TextColored(done ? kDone : (partly ? kPartly : kMuted), "%s %s",
                           done ? "+" : (partly ? "~" : "-"), name);
        ImGui::SameLine();
    }

    static void draw_status(const VideoEntry& video) {
        ImGui::Indent();
        step("align", video.aligned);
        step("labels", video.annotation == AnnotationProgress::Complete,
             video.annotation == AnnotationProgress::InProgress);
        step("features", !video.features_model.empty());
        step("detect", video.detected);
        step("refine", video.refinement == AnnotationProgress::Complete,
             video.refinement == AnnotationProgress::InProgress);
        step("cut", !video.cuts.empty());
        ImGui::NewLine();
        ImGui::Unindent();
    }

    /// "Name: file name", with the full path as tooltip.
    static void file_line(const char* name, const std::string& path) {
        ImGui::TextWrapped("%s: %s", name,
                           std::filesystem::path(path).filename().string().c_str());
        ImGui::SetItemTooltip("%s", path.c_str());
    }

    /// How many of the match day's videos have each step done.
    void draw_match_day_details(const std::string& match_day) {
        ImGui::Text("Match day %s", match_day.c_str());
        std::vector<const VideoEntry*> videos;
        for (const VideoEntry& video : session_.workspace().videos) {
            if (video.match_day == match_day) {
                videos.push_back(&video);
            }
        }
        for (const std::string_view command : launcher::kPipelineSteps) {
            const auto done = std::ranges::count_if(videos, [&](const VideoEntry* video) {
                return launcher::step_done(command, *video);
            });
            ImGui::TextColored(done == std::ssize(videos) ? kDone : (done > 0 ? kPartly : kMuted),
                               "%.*s: %td of %zu games", static_cast<int>(command.size()),
                               command.data(), done, videos.size());
        }
        const auto complete = std::ranges::count_if(videos, [](const VideoEntry* video) {
            return video->annotation == AnnotationProgress::Complete;
        });
        ImGui::TextColored(kMuted, "complete labels: %td of %zu games", complete, videos.size());
    }

    void draw_details(const VideoEntry& video) {
        file_line("Original", video.original);
        if (video.cut_candidates.empty()) {
            ImGui::TextColored(kMuted, "No pre-cut version (needed by align)");
        }
        for (const std::string& cut : video.cut_candidates) {
            file_line("Pre-cut", cut);
        }
        if (scenes_ != nullptr) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Scene:");
            ImGui::SameLine();
            draw_scene_choice(video);
        }
        const char* labels = video.annotation == AnnotationProgress::Complete     ? "complete"
                             : video.annotation == AnnotationProgress::InProgress ? "in progress"
                                                                                   : "none";
        ImGui::Text("Labels: %s (%zu rallies)", labels, video.rallies);
        ImGui::TextWrapped("Features: %s", video.features_model.empty()
                                               ? "none"
                                               : video.features_model.c_str());
        for (const std::string& cut : video.cuts) {
            file_line("Cut", cut);
        }
        ImGui::TextColored(kMuted, "+ done   ~ in progress   - not yet");
    }

    // ------------------------------------------------------------------ forms

    void draw_forms() {
        auto& forms = session_.forms();
        if (!ImGui::BeginTabBar("##tabs")) {
            return;
        }
        bool highlights_shown = false;
        const std::optional<std::string>& day = session_.selected_match_day();
        if (day && ImGui::BeginTabItem("Merge", nullptr,
                                       first_tab_ == kMergeTab ? ImGuiTabItemFlags_SetSelected
                                                               : 0)) {
            ImGui::BeginChild("##tab", {0, 0});
            draw_merge(*day);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (day && ImGui::BeginTabItem("Pipeline", nullptr,
                                       first_tab_ == kPipelineTab ? ImGuiTabItemFlags_SetSelected
                                                                  : 0)) {
            ImGui::BeginChild("##tab", {0, 0});
            draw_pipeline(*day);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        for (std::size_t f = 0; f < forms.size(); ++f) {
            const bool select = !first_tab_.empty() && forms[f].name == first_tab_;
            if (ImGui::BeginTabItem(forms[f].title.c_str(), nullptr,
                                    select ? ImGuiTabItemFlags_SetSelected : 0)) {
                // The tab's content scrolls below the tabs, which stay in place
                ImGui::BeginChild("##tab", {0, 0});
                const bool reopened = !highlights_shown_ || shown_day_ != day;
                if (forms[f].name == kHighlightsForm && !highlights_shown_) {
                    fit_frames_ = kFitFrames;
                }
                if (forms[f].name == kHighlightsForm) {
                    draw_highlights(forms[f], f, reopened);
                    highlights_shown = true;
                    shown_day_ = day;
                } else if (day && launcher::supports_batch(forms[f].name)) {
                    draw_match_day_form(forms[f], f, *day);
                } else if (day && forms[f].name != kTrainForm) {
                    ImGui::TextWrapped("%s", forms[f].description.c_str());
                    ImGui::Spacing();
                    ImGui::TextColored(kMuted, "%s works on one game at a time: choose a game on "
                                               "the left.",
                                       forms[f].title.c_str());
                } else {
                    draw_form(forms[f], f);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        highlights_shown_ = highlights_shown;
        first_tab_.clear();
        ImGui::EndTabBar();
    }

    // ------------------------------------------------------------------ highlights

    /// The highlight choice of the selected video's game, then the settings of the videos and
    /// the buttons that make them. `opened`: the tab was not shown before (read the rallies).
    void draw_highlights(CommandForm& form, std::size_t form_index, bool opened) {
        ImGui::TextWrapped("%s", form.description.c_str());
        const auto selected = session_.selected_video();
        if (!selected) {
            ImGui::TextColored(kMuted, "Select a video on the left.");
            return;
        }
        if (!highlights_) {
            ImGui::TextColored(kError, "Highlights are not available in this window.");
            return;
        }
        const VideoEntry& video = session_.workspace().videos[*selected];
        const std::optional<std::string>& day = session_.selected_match_day();
        if (day) {
            highlights_->draw_match_day(session_.workspace(), *day, opened, margins_of(form),
                                        crossfade_of(form));
        } else {
            highlights_->draw(session_.workspace(), video, opened, margins_of(form),
                              crossfade_of(form));
        }
        ImGui::SeparatorText("Highlight videos");
        auto set_by_buttons = [](const Field& field) {
            return field.flag == kVideoIdFlag || field.flag == kMatchDayFlag;
        };
        for (std::size_t i = 0; i < form.fields.size(); ++i) {
            if (!form.fields[i].advanced && !set_by_buttons(form.fields[i])) {
                draw_field(form.fields[i], form_index, i);
            }
        }
        if (ImGui::CollapsingHeader("Advanced")) {
            for (std::size_t i = 0; i < form.fields.size(); ++i) {
                if (form.fields[i].advanced && !set_by_buttons(form.fields[i])) {
                    draw_field(form.fields[i], form_index, i);
                }
            }
        }
        ImGui::Separator();
        draw_highlight_buttons(form, video);
    }

    /// The margins played around a rally: those of the form, else the command's defaults.
    static cutting::Margins margins_of(CommandForm& form) {
        cutting::Margins margins;
        auto read = [&](const char* flag, double& value) {
            if (const Field* field = form.field(flag); field != nullptr && !field->value.empty()) {
                try {
                    value = std::max(0.0, std::stod(field->value));
                } catch (const std::exception&) {
                    // keep the default while the field is being edited
                }
            }
        };
        read("--before", margins.before_s);
        read("--after", margins.after_s);
        return margins;
    }

    /// The cross-fade of the form, else the command's default.
    static double crossfade_of(CommandForm& form) {
        const Field* field = form.field("--crossfade");
        if (field != nullptr) {
            try {
                return std::max(0.0, std::stod(field->effective_value()));
            } catch (const std::exception&) {
                // the default while the field is being edited
            }
        }
        return kDefaultCrossfade;
    }

    void draw_highlight_buttons(const CommandForm& form, const VideoEntry& video) {
        const HighlightCounts counts = highlights_->counts();
        auto run = [&](bool match_day) {
            CommandForm call = form;
            // No video id makes the match day video: not a fallback to the game's id
            Field& video_id = *call.field(kVideoIdFlag);
            video_id.value = match_day ? "" : video.video_id;
            video_id.initial_value = video_id.value;
            call.field(kMatchDayFlag)->value = match_day ? "1" : "";
            try {
                session_.run(call);
                error_.clear();
            } catch (const std::exception& error) {
                error_ = error.what();
            }
        };
        if (!session_.selected_match_day()) {
            ImGui::BeginDisabled(session_.running() || counts.game == 0);
            if (ImGui::Button(std::format("Make game video ({} rallies)", counts.game).c_str())) {
                run(false);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The game highlights of %s, with its title",
                                  video.video_id.c_str());
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(session_.running() || counts.match_day == 0);
        if (ImGui::Button(
                std::format("Make match day video ({} rallies)", counts.match_day).c_str())) {
            run(true);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The match day highlights of all games of %s",
                              video.match_day.c_str());
        if (session_.running()) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {100, 0})) {
                session_.cancel();
            }
        }
        if (!error_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kError, "%s", error_.c_str());
        }
        ImGui::TextColored(kMuted, "Written to %s",
                           (std::filesystem::path(video.work_dir) / "highlights").string().c_str());
    }

    void draw_form(CommandForm& form, std::size_t form_index) {
        ImGui::TextWrapped("%s", form.description.c_str());
        ImGui::Spacing();
        for (std::size_t i = 0; i < form.fields.size(); ++i) {
            if (!form.fields[i].advanced) {
                draw_field(form.fields[i], form_index, i);
            }
        }
        if (ImGui::CollapsingHeader("Advanced")) {
            if (form.program == launcher::Program::Uv) {
                ImGui::PushID("program");
                label_cell("uv program", "The uv executable that runs the training");
                input_text("##value", form.program_path);
                ImGui::PopID();
            }
            for (std::size_t i = 0; i < form.fields.size(); ++i) {
                if (form.fields[i].advanced) {
                    draw_field(form.fields[i], form_index, i);
                }
            }
        }
        ImGui::Separator();
        draw_run(form);
    }

    static void label_cell(const std::string& label, const std::string& help) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        if (!help.empty()) {
            ImGui::SetItemTooltip("%s", help.c_str());
        }
        ImGui::SameLine(kLabelWidth);
    }

    void draw_field(Field& field, std::size_t form_index, std::size_t field_index) {
        ImGui::PushID(static_cast<int>(field_index));
        label_cell(field.label + (field.required ? " *" : ""), field.help);
        switch (field.kind) {
        case FieldKind::Flag: {
            bool on = !field.value.empty();
            if (ImGui::Checkbox("##value", &on)) {
                field.value = on ? "1" : "";
            }
            break;
        }
        case FieldKind::Choice:
            if (draw_choice(field)) {
                session_.field_edited(session_.forms()[form_index], field);
            }
            break;
        case FieldKind::MultiChoice:
            draw_multi_choice(field);
            break;
        default:
            draw_text(field, form_index, field_index);
        }
        ImGui::PopID();
    }

    void draw_text(Field& field, std::size_t form_index, std::size_t field_index) {
        const bool browse = field.kind == FieldKind::File || field.kind == FieldKind::Directory;
        const bool number = field.kind == FieldKind::Number;
        const float buttons = (browse ? 32.0F : 0.0F) + (field.suggestions.empty() ? 0.0F : 32.0F);
        ImGui::SetNextItemWidth(number ? number_width(field.value)
                                       : std::max(120.0F, ImGui::GetContentRegionAvail().x -
                                                              buttons));
        // An emptied field stands for the proposed value: shown as the hint
        const std::string& used = field.effective_value();
        const std::string hint =
            used.empty() ? (number ? "auto" : "") : (number ? used : "default: " + used);
        input_text("##value", field.value, ImGuiInputTextFlags_None, hint.c_str());
        bool edited = ImGui::IsItemDeactivatedAfterEdit();
        if (!field.help.empty()) {
            ImGui::SetItemTooltip("%s", field.help.c_str());
        }
        if (!field.suggestions.empty()) {
            ImGui::SameLine();
            if (ImGui::BeginCombo("##suggestions", nullptr, ImGuiComboFlags_NoPreview |
                                                               ImGuiComboFlags_PopupAlignLeft)) {
                for (const std::string& suggestion : field.suggestions) {
                    if (ImGui::Selectable(suggestion.c_str(), suggestion == field.value)) {
                        field.value = suggestion;
                        edited = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("Suggestions");
        }
        if (browse) {
            ImGui::SameLine();
            if (ImGui::Button("...")) {
                open_dialog(field, form_index, field_index);
            }
            ImGui::SetItemTooltip("Choose");
        }
        if (edited) { // after the suggestion loop: this may update the suggestions
            session_.field_edited(session_.forms()[form_index], field);
        }
    }

    /// Wide enough for a number of kNumberFieldDigits digits, or the value if longer.
    static float number_width(const std::string& value) {
        const std::string widest(std::max(kNumberFieldDigits, value.size()), '0');
        return ImGui::CalcTextSize(widest.c_str()).x + 2.0F * ImGui::GetStyle().FramePadding.x;
    }

    /// Returns whether another choice was made.
    static bool draw_choice(Field& field) {
        const auto help_of = [&](std::size_t i) -> const std::string* {
            return i < field.choice_help.size() && !field.choice_help[i].empty()
                       ? &field.choice_help[i]
                       : nullptr;
        };
        ImGui::SetNextItemWidth(200);
        const bool open = ImGui::BeginCombo("##value", field.value.c_str());
        if (!open) {
            const auto chosen = std::ranges::find(field.choices, field.value);
            const std::string* help =
                help_of(static_cast<std::size_t>(chosen - field.choices.begin()));
            if (help != nullptr) {
                wrapped_tooltip(*help);
            }
            return false;
        }
        bool changed = false;
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            const std::string& choice = field.choices[i];
            if (ImGui::Selectable(choice.c_str(), choice == field.value) &&
                choice != field.value) {
                field.value = choice;
                changed = true;
            }
            if (const std::string* help = help_of(i)) {
                wrapped_tooltip(*help);
            }
        }
        ImGui::EndCombo();
        return changed;
    }

    /// Tooltip of the last item, wrapped to a readable width.
    static void wrapped_tooltip(const std::string& text) {
        constexpr float kWidthInFontSizes = 30.0F;
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * kWidthInFontSizes);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    void collect_thumbnails() {
        for (auto& [video, frame] : thumbnail_worker_.take_results()) {
            if (!frame) {
                continue; // stays a placeholder
            }
            frame->index = 0;
            auto texture = std::make_unique<FrameTexture>(gui_.renderer());
            texture->show(*frame);
            thumbnails_[video] = std::move(texture);
        }
    }

    // ------------------------------------------------------------------ scenes

    /// Suggests the scenes of all videos of a match day in the background.
    void start_scene_detection(const std::string& match_day) {
        std::vector<std::filesystem::path> videos;
        scene_job_ids_.clear();
        for (const VideoEntry& video : session_.workspace().videos) {
            if (video.match_day == match_day) {
                videos.emplace_back(video.original);
                scene_job_ids_.push_back(video.video_id);
                scene_job_work_ = video.work_dir;
            }
        }
        scene_job_day_ = match_day;
        scene_job_ = std::async(std::launch::async,
                                [&preview = preview_, videos = std::move(videos)] {
                                    return launcher::detect_scenes(preview, videos);
                                });
    }

    [[nodiscard]] bool detecting_scenes() const { return scene_job_.valid(); }

    void collect_scenes() {
        if (!scene_job_.valid() ||
            scene_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        try {
            const std::vector<std::string> found = scene_job_.get();
            auto stored = scenes_->load(scene_job_work_);
            for (std::size_t i = 0; i < found.size() && i < scene_job_ids_.size(); ++i) {
                stored[scene_job_ids_[i]] = found[i];
            }
            scenes_->save(scene_job_work_, stored);
            error_.clear();
        } catch (const std::exception& error) {
            error_ = std::format("Scenes of {}: {}", scene_job_day_, error.what());
        }
        scene_job_day_.clear();
        session_.rescan();
    }

    void set_scene(const VideoEntry& video, const std::string& scene) {
        try {
            auto stored = scenes_->load(video.work_dir);
            if (scene == kNoScene) {
                stored.erase(video.video_id);
            } else {
                stored[video.video_id] = scene;
            }
            scenes_->save(video.work_dir, stored);
            error_.clear();
        } catch (const std::exception& error) {
            error_ = error.what();
        }
        session_.rescan();
    }

    /// The scenes used on a match day plus a new one.
    [[nodiscard]] std::vector<std::string> scene_choices(const std::string& match_day) const {
        std::vector<std::string> used;
        for (const VideoEntry& video : session_.workspace().videos) {
            if (video.match_day == match_day && !video.scene.empty() &&
                std::ranges::find(used, video.scene) == used.end()) {
                used.push_back(video.scene);
            }
        }
        std::ranges::sort(used);
        std::size_t next = 0;
        while (std::ranges::find(used, launcher::scene_name(next)) != used.end()) {
            ++next;
        }
        used.push_back(launcher::scene_name(next));
        used.insert(used.begin(), kNoScene);
        return used;
    }

    void draw_match_day_header(const std::string& match_day) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "%s", match_day.c_str());
        ImGui::SameLine();
        draw_scene_detection(match_day);
    }

    /// "Detect scenes" for a match day, or a note while it runs.
    void draw_scene_detection(const std::string& match_day) {
        if (scenes_ == nullptr) {
            return;
        }
        ImGui::PushID(match_day.c_str());
        ImGui::PushID("detect");
        draw_scene_detection_button(match_day);
        ImGui::PopID();
        ImGui::PopID();
    }

    void draw_scene_detection_button(const std::string& match_day) {
        if (scene_job_day_ == match_day) {
            ImGui::TextColored(kPartly, "detecting the scenes...");
            return;
        }
        ImGui::BeginDisabled(detecting_scenes());
        if (ImGui::SmallButton("Detect scenes")) {
            start_scene_detection(match_day);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Suggest the scenes of this match day's videos from their "
                              "backgrounds (same hall and camera position = same scene); takes "
                              "a few seconds per video");
    }

    void draw_scene_choice(const VideoEntry& video) {
        ImGui::SetNextItemWidth(kSceneComboWidth);
        const char* current = video.scene.empty() ? kNoScene : video.scene.c_str();
        if (ImGui::BeginCombo("##scene", current)) {
            for (const std::string& scene : scene_choices(video.match_day)) {
                if (ImGui::Selectable(scene.c_str(), scene == current)) {
                    set_scene(video, scene);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Scene: videos with the same hall and camera position");
    }

    /// The workspace video with this key ("<match day>/<video id>"), if any.
    [[nodiscard]] std::optional<std::size_t> video_with_key(const std::string& key) const {
        const auto& videos = session_.workspace().videos;
        for (std::size_t i = 0; i < videos.size(); ++i) {
            if (videos[i].key() == key) {
                return i;
            }
        }
        return std::nullopt;
    }

    /// A video's thumbnail (requested on first use); clicking it selects the video.
    void draw_thumbnail(std::size_t video_index) {
        const std::string& original = session_.workspace().videos[video_index].original;
        if (thumbnails_requested_.insert(original).second) {
            thumbnail_worker_.request(original);
        }
        const ImVec2 size{kThumbnailWidth, static_cast<float>(kThumbnailHeight)};
        const auto found = thumbnails_.find(original);
        if (found != thumbnails_.end() && found->second->texture() != nullptr) {
            ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(found->second->texture())),
                         size);
        } else {
            ImGui::Dummy(size);
            ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                                      ImGui::GetItemRectMax(),
                                                      ImGui::GetColorU32(ImGuiCol_FrameBg));
        }
        if (ImGui::IsItemClicked() && !session_.running()) {
            session_.select_video(video_index);
        }
        ImGui::SetItemTooltip("Show this video on the left");
    }

    void draw_multi_choice(Field& field) {
        if (field.choices.empty()) {
            ImGui::TextColored(kMuted, "none yet (complete labels and features are needed)");
            return;
        }
        std::vector<std::string> selected = lines_of(field.value);
        bool changed = false;
        ImGui::BeginGroup();
        std::string group;
        for (std::size_t i = 0; i < field.choices.size(); ++i) {
            const std::string& choice = field.choices[i];
            const auto [match_day, video_id] = split_key(choice);
            if (i == 0 || match_day != group) {
                group = match_day;
                ImGui::PushID(group.c_str());
                draw_match_day_header(group);
                ImGui::PopID();
            }
            ImGui::PushID(static_cast<int>(i));
            bool on = std::ranges::find(selected, choice) != selected.end();
            ImGui::Indent();
            const auto video = video_with_key(choice);
            bool clicked = false;
            if (video) {
                clicked = ImGui::Checkbox("##tick", &on);
                ImGui::SameLine();
                draw_thumbnail(*video);
                if (scenes_ != nullptr) {
                    ImGui::SameLine();
                    draw_scene_choice(session_.workspace().videos[*video]);
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                if (ImGui::Selectable(video_id.c_str(), false,
                                      ImGuiSelectableFlags_None)) {
                    on = !on;
                    clicked = true;
                }
            } else {
                clicked = ImGui::Checkbox(video_id.c_str(), &on);
            }
            ImGui::Unindent();
            ImGui::PopID();
            if (clicked) {
                changed = true;
                if (on) {
                    selected.push_back(choice);
                } else {
                    std::erase(selected, choice);
                }
            }
        }
        ImGui::EndGroup();
        if (changed) {
            field.value.clear();
            for (const std::string& choice : field.choices) { // keep the listed order
                if (std::ranges::find(selected, choice) != selected.end()) {
                    field.value += choice + "\n";
                }
            }
        }
        for (const std::string& scene :
             launcher::scenes_in_both(session_.workspace().videos, field.choices,
                                      lines_of(field.value))) {
            ImGui::TextColored(kPartly, "%s is in training and validation: the validation "
                                        "flatters the model", scene.c_str());
        }
    }

    void draw_run(const CommandForm& form) {
        const launcher::Invocation call = session_.invocation(form);
        std::string line = launcher::command_line(call.program, call.arguments);
        ImGui::TextColored(kMuted, "Command line (run in %s):",
                           form.working_directory.empty() ? "the project directory"
                                                          : form.working_directory.c_str());
        ImGui::SetNextItemWidth(-70);
        input_text("##command", line, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(line.c_str());
        }
        const auto missing = form.missing();
        ImGui::BeginDisabled(session_.running() || !missing.empty());
        if (ImGui::Button(std::format("Run {}", form.name).c_str(), {160, 0})) {
            try {
                session_.run(form);
                error_.clear();
            } catch (const std::exception& error) {
                error_ = error.what();
            }
        }
        ImGui::EndDisabled();
        if (session_.running()) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {100, 0})) {
                session_.cancel();
            }
        }
        if (!missing.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kError, "Fill in: %s", missing.front().c_str());
        } else if (!error_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kError, "%s", error_.c_str());
        }
    }

    /// A command's form for the match day as a whole: only the settings that are the same for
    /// every game (they are used for each), then "Compute all" and "Compute missing".
    void draw_match_day_form(CommandForm& form, std::size_t form_index, const std::string& day) {
        ImGui::TextWrapped("%s", form.description.c_str());
        ImGui::TextColored(kMuted, "For every game of %s, one after another; the paths of each "
                                   "game are filled in for it.",
                           day.c_str());
        ImGui::Spacing();
        const auto selected = session_.selected_video();
        const std::vector<bool> dependent =
            selected ? launcher::video_dependent_fields(
                           form.name, session_.workspace().videos[*selected], session_.workspace())
                     : std::vector<bool>(form.fields.size(), false);
        const auto own = [&](std::size_t i) { return i < dependent.size() && dependent[i]; };
        for (std::size_t i = 0; i < form.fields.size(); ++i) {
            if (!form.fields[i].advanced && !own(i)) {
                draw_field(form.fields[i], form_index, i);
            }
        }
        if (ImGui::CollapsingHeader("Advanced")) {
            for (std::size_t i = 0; i < form.fields.size(); ++i) {
                if (form.fields[i].advanced && !own(i)) {
                    draw_field(form.fields[i], form_index, i);
                }
            }
        }
        ImGui::Separator();
        draw_batch(form, day);
        draw_cancel_and_error();
    }

    void draw_cancel_and_error() {
        if (session_.running()) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", {100, 0})) {
                session_.cancel();
            }
        }
        if (!error_.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kError, "%s", error_.c_str());
        }
    }

    /// "Compute all" and "Compute missing" for every game of the match day.
    void draw_batch(const CommandForm& form, const std::string& day) {
        std::size_t total = 0;
        std::size_t open = 0;
        for (const VideoEntry& video : session_.workspace().videos) {
            if (video.match_day == day) {
                ++total;
                open += launcher::step_done(form.name, video) ? 0 : 1;
            }
        }
        auto run = [&](launcher::BatchScope scope) {
            try {
                session_.run_batch(form, scope);
                error_.clear();
            } catch (const std::exception& error) {
                error_ = error.what();
            }
        };
        ImGui::BeginDisabled(session_.running());
        if (ImGui::Button(std::format("Compute all ({})", total).c_str())) {
            run(launcher::BatchScope::All);
        }
        ImGui::SetItemTooltip("Runs %s again for every game of %s, one after another, with the "
                              "settings above; games without its input are skipped",
                              form.name.c_str(), day.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(open == 0);
        if (ImGui::Button(std::format("Compute missing ({})", open).c_str())) {
            run(launcher::BatchScope::Missing);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Runs %s only for the games of %s that do not have its result yet",
                              form.name.c_str(), day.c_str());
        ImGui::EndDisabled();
    }

    /// Parts of a game recorded in several files, ticked among the match day's videos, put in
    /// order and appended into a new video.
    void draw_merge(const std::string& day) {
        if (merge_day_ != day) {
            merge_day_ = day;
            merge_parts_.clear();
            merge_output_.clear();
            merge_output_edited_ = false;
        }
        ImGui::TextWrapped("Append the parts of a game recorded in several files (e.g. a camera "
                           "that starts a new file every few gigabytes) into one new video, "
                           "without re-encoding: takes about as long as copying the files. The "
                           "parts stay where they are and stay in the list; move them out of "
                           "the match day folder yourself once the merged video is checked.");
        ImGui::Spacing();
        const auto& videos = session_.workspace().videos;
        std::erase_if(merge_parts_, [&](const std::string& part) {
            return std::ranges::find(videos, part, &VideoEntry::original) == videos.end();
        });
        ImGui::SeparatorText("Parts");
        ImGui::BeginDisabled(session_.running());
        for (const VideoEntry& video : videos) {
            if (video.match_day != day) {
                continue;
            }
            ImGui::PushID(video.original.c_str());
            bool ticked = std::ranges::find(merge_parts_, video.original) != merge_parts_.end();
            if (ImGui::Checkbox(std::filesystem::path(video.original).filename().string().c_str(),
                                &ticked)) {
                if (ticked) {
                    // In the list's order (by file name), which is usually the recording order
                    const auto later = std::ranges::find_if(
                        merge_parts_, [&](const std::string& p) { return p > video.original; });
                    merge_parts_.insert(later, video.original);
                } else {
                    std::erase(merge_parts_, video.original);
                }
            }
            ImGui::SetItemTooltip("%s", video.original.c_str());
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (!merge_parts_.empty()) {
            draw_merge_order();
        }
        if (!merge_output_edited_) {
            merge_output_ = merge_parts_.empty() ? std::string()
                                                 : launcher::merged_name(merge_parts_.front());
        }
        ImGui::SeparatorText("Merged video");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (input_text("##merged", merge_output_)) {
            merge_output_edited_ = !merge_output_.empty();
        }
        ImGui::SetItemTooltip("A new file (an existing one is never replaced); proposed next to "
                              "the first part. The video id is suggested from its name");
        const CommandForm form = launcher::merge_form(merge_parts_, merge_output_);
        const launcher::Invocation call = session_.invocation(form);
        std::string line = launcher::command_line(call.program, call.arguments);
        ImGui::TextColored(kMuted, "Command line:");
        ImGui::SetNextItemWidth(-70);
        input_text("##command", line, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(line.c_str());
        }
        ImGui::BeginDisabled(session_.running() || merge_parts_.size() < 2 ||
                             merge_output_.empty());
        if (ImGui::Button(std::format("Merge {} parts", merge_parts_.size()).c_str(), {160, 0})) {
            try {
                session_.run(form);
                error_.clear();
            } catch (const std::exception& error) {
                error_ = error.what();
            }
        }
        ImGui::EndDisabled();
        if (merge_parts_.size() < 2) {
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "Tick at least two parts");
        }
        draw_cancel_and_error();
    }

    /// The ticked parts in the order they are appended, each movable up and down.
    void draw_merge_order() {
        ImGui::SeparatorText("Order");
        ImGui::BeginDisabled(session_.running());
        const auto& videos = session_.workspace().videos;
        std::optional<std::pair<std::size_t, std::size_t>> swap;
        for (std::size_t i = 0; i < merge_parts_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                swap = {i - 1, i};
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == merge_parts_.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                swap = {i, i + 1};
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%zu. %s", i + 1,
                        std::filesystem::path(merge_parts_[i]).filename().string().c_str());
            const auto video = std::ranges::find(videos, merge_parts_[i], &VideoEntry::original);
            if (video != videos.end() && has_results(*video)) {
                ImGui::SameLine();
                ImGui::TextColored(kPartly, "(has results already: they stay with the part)");
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (swap) {
            std::swap(merge_parts_[swap->first], merge_parts_[swap->second]);
        }
    }

    static bool has_results(const VideoEntry& video) {
        return video.aligned || video.annotation != AnnotationProgress::None ||
               !video.features_model.empty() || video.detected || !video.cuts.empty();
    }

    /// The steps to run for every game of the match day, and the buttons that run them.
    void draw_pipeline(const std::string& day) {
        ImGui::TextWrapped("Run the ticked steps for every game of %s: all steps of one game, "
                           "then the next game. Each step uses the settings of its own tab. A "
                           "failed step skips the rest of its game.",
                           day.c_str());
        ImGui::Spacing();
        std::vector<std::string>& steps = session_.pipeline_steps();
        std::vector<const VideoEntry*> videos;
        for (const VideoEntry& video : session_.workspace().videos) {
            if (video.match_day == day) {
                videos.push_back(&video);
            }
        }
        ImGui::BeginDisabled(session_.running());
        bool first = true;
        for (const std::string_view step : launcher::kPipelineSteps) {
            if (!first) {
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "->");
                ImGui::SameLine();
            }
            first = false;
            const std::string name(step);
            bool ticked = std::ranges::find(steps, name) != steps.end();
            if (ImGui::Checkbox(name.c_str(), &ticked)) {
                if (ticked) {
                    steps.push_back(name);
                } else {
                    std::erase(steps, name);
                }
            }
            const auto done = std::ranges::count_if(
                videos, [&](const VideoEntry* video) { return launcher::step_done(step, *video); });
            ImGui::SetItemTooltip("Done for %td of %zu games", done, videos.size());
        }
        ImGui::SameLine(0, 30);
        if (ImGui::SmallButton("Default")) {
            steps = launcher::default_pipeline();
        }
        ImGui::SetItemTooltip("features -> detect -> cut");
        ImGui::EndDisabled();
        ImGui::Spacing();
        std::size_t all = 0;
        std::size_t missing = 0;
        if (ImGui::BeginTable("##plan", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (const VideoEntry* video : videos) {
                const auto planned =
                    launcher::pipeline_for(steps, *video, launcher::BatchScope::Missing);
                all += launcher::pipeline_for(steps, *video, launcher::BatchScope::All).size();
                missing += planned.size();
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(video->video_id.c_str());
                ImGui::TableNextColumn();
                std::string text;
                for (const std::string& step : planned) {
                    text += (text.empty() ? "" : ", ") + step;
                }
                ImGui::TextColored(planned.empty() ? kDone : kPartly, "%s",
                                   planned.empty() ? "done" : ("missing: " + text).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        auto run = [&](launcher::BatchScope scope) {
            try {
                session_.run_pipeline(scope);
                error_.clear();
            } catch (const std::exception& error) {
                error_ = error.what();
            }
        };
        ImGui::BeginDisabled(session_.running() || all == 0);
        if (ImGui::Button(std::format("Compute all ({} steps)", all).c_str())) {
            run(launcher::BatchScope::All);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Runs every ticked step again for every game");
        ImGui::SameLine();
        ImGui::BeginDisabled(session_.running() || missing == 0);
        if (ImGui::Button(std::format("Compute missing ({} steps)", missing).c_str())) {
            run(launcher::BatchScope::Missing);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Per game from the first ticked step without its result on (the "
                              "later steps would be out of date otherwise)");
        draw_cancel_and_error();
    }

    // ------------------------------------------------------------------ output

    void draw_log() {
        const std::string& status = session_.status();
        const bool failed = status.find("failed") != std::string::npos;
        ImGui::TextColored(failed ? kError : (session_.running() ? kPartly : kDone), "%s",
                           status.empty() ? "Output" : status.c_str());
        ImGui::BeginChild("##log", {0, 0}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(session_.log().c_str());
        if (session_.log().size() != shown_log_size_) {
            shown_log_size_ = session_.log().size();
            ImGui::SetScrollHereY(1.0F); // follow new output
        }
        ImGui::EndChild();
    }

    // ------------------------------------------------------------------ file dialogs

    void open_dialog(const Field& field, std::size_t form_index, std::size_t field_index) {
        pending_target_ = {form_index, field_index, {}};
        const std::filesystem::path library(session_.workspace().library);
        const std::string start =
            field.value.empty() ? library.string()
                                : (project_ / field.value).parent_path().string();
        if (field.kind == FieldKind::Directory) {
            SDL_ShowOpenFolderDialog(&LauncherApp::on_dialog, this, gui_.window(), start.c_str(),
                                     false);
        } else if (is_output(field)) {
            SDL_ShowSaveFileDialog(&LauncherApp::on_dialog, this, gui_.window(), nullptr, 0,
                                   start.c_str());
        } else {
            SDL_ShowOpenFileDialog(&LauncherApp::on_dialog, this, gui_.window(), nullptr, 0,
                                   start.c_str(), false);
        }
    }

    static void SDLCALL on_dialog(void* user, const char* const* files, int /*filter*/) {
        auto* app = static_cast<LauncherApp*>(user);
        if (files == nullptr || files[0] == nullptr) {
            return; // cancelled or failed
        }
        const std::lock_guard lock(app->dialog_mutex_);
        app->dialog_result_ = app->pending_target_;
        app->dialog_result_->path = files[0];
    }

    void apply_dialog_result() {
        std::optional<DialogResult> result;
        {
            const std::lock_guard lock(dialog_mutex_);
            result = std::exchange(dialog_result_, std::nullopt);
        }
        if (result && result->form == kLibraryTarget) {
            choose_library(result->path);
            return;
        }
        auto& forms = session_.forms();
        if (!result || result->form >= forms.size() ||
            result->field >= forms[result->form].fields.size()) {
            return;
        }
        const std::filesystem::path chosen(result->path);
        const auto relative = chosen.lexically_relative(project_);
        const bool inside = !relative.empty() && *relative.begin() != "..";
        Field& field = forms[result->form].fields[result->field];
        field.value = inside ? relative.generic_string() : chosen.string();
        session_.field_edited(forms[result->form], field);
    }

    void choose_library(const std::filesystem::path& library) {
        try {
            session_.set_library(library);
            if (settings_ != nullptr) {
                settings_->save_library(library);
            }
            error_.clear();
        } catch (const std::exception& error) {
            error_ = error.what();
        }
    }

    launcher::LauncherSession& session_;
    launcher::VideoPreview& preview_;
    const GuiContext& gui_;
    std::filesystem::path project_;
    launcher::LauncherSettings* settings_;
    launcher::SceneStore* scenes_;
    std::unique_ptr<HighlightsPanel> highlights_; ///< nullptr: not available
    bool highlights_shown_ = false; ///< the tab was shown in the last frame
    float log_share_ = kLogShare;   ///< of the right side's height taken by the output
    int fit_frames_ = 0; ///< frames left in which the output is fitted to the highlights player
    std::optional<std::string> shown_day_; ///< match day chosen as a whole when it was shown
    std::string merge_day_;                ///< match day of the merge choice below
    std::vector<std::string> merge_parts_; ///< originals ticked to merge, in their order
    std::string merge_output_;
    bool merge_output_edited_ = false; ///< else the output follows the first part
    std::string first_tab_;         ///< selected in the first frame, then empty
    /// The selection last saved as the last video; a change opens its match day.
    std::optional<std::size_t> remembered_;
    std::future<std::vector<std::string>> scene_job_; ///< closing waits for a running detection
    std::string scene_job_day_;
    std::vector<std::string> scene_job_ids_;
    std::filesystem::path scene_job_work_;
    std::string error_;
    std::size_t shown_log_size_ = 0;
    DialogResult pending_target_;
    std::mutex dialog_mutex_;
    std::optional<DialogResult> dialog_result_;
    PreviewWorker preview_worker_;
    FrameTexture preview_texture_;
    ThumbnailWorker thumbnail_worker_;
    std::map<std::string, std::unique_ptr<FrameTexture>> thumbnails_; ///< by original video
    std::set<std::string> thumbnails_requested_;
    std::string preview_video_;
    int preview_percent_ = kDefaultPreviewPercent;
    std::int64_t preview_count_ = 0;
    double preview_time_ = 0.0;
    bool preview_shown_ = false;
    bool preview_failed_ = false;
};

} // namespace

void run_launcher(launcher::LauncherSession& session, launcher::VideoPreview& preview,
                  const LauncherOptions& options) {
    const GuiContext gui(options.title, 1500, 950);
    LauncherApp app(session, preview, gui, options);
    for (int rendered = 0; options.max_frames == 0 || rendered < options.max_frames; ++rendered) {
        if (gui.poll_events()) {
            break;
        }
        gui.begin_frame();
        app.draw();
        const bool last = options.max_frames != 0 && rendered + 1 == options.max_frames;
        gui.end_frame(last ? options.screenshot : std::filesystem::path{});
    }
    session.cancel();
}

} // namespace ttrally::gui
