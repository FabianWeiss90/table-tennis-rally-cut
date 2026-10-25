// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/launcher_app.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "launcher/domain/scenes.hpp"
#include "shared/io/formatting.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
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

constexpr float kSidebarWidth = 380.0F;
constexpr float kLabelWidth = 230.0F;
constexpr float kLogShare = 0.38F; ///< of the right side's height
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
                const GuiContext& gui, std::filesystem::path project,
                launcher::LauncherSettings* settings, launcher::SceneStore* scenes)
        : session_(session), preview_(preview), gui_(gui),
          project_(std::filesystem::absolute(project)), settings_(settings), scenes_(scenes),
          preview_worker_(preview), preview_texture_(gui.renderer()), thumbnail_worker_(preview) {}

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
        ImGui::BeginChild("##videos", {kSidebarWidth, 0}, ImGuiChildFlags_Borders);
        draw_videos();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##commands", {0, 0});
        const float log_height = ImGui::GetContentRegionAvail().y * kLogShare;
        ImGui::BeginChild("##forms", {0, -log_height});
        draw_forms();
        ImGui::EndChild();
        draw_log();
        ImGui::EndChild();
        ImGui::End();
    }

  private:
    // ------------------------------------------------------------------ videos

    void draw_library() {
        ImGui::TextUnformatted("Library");
        ImGui::SameLine(kSidebarWidth - 140);
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
        ImGui::BeginChild("##list", {0, selected_any ? -kSelectedVideoHeight : 0});
        for (std::size_t i = 0; i < videos.size(); ++i) {
            const VideoEntry& video = videos[i];
            if (i == 0 || video.match_day != videos[i - 1].match_day) {
                ImGui::SeparatorText(video.match_day.c_str());
                draw_scene_detection(video.match_day);
            }
            ImGui::PushID(static_cast<int>(i));
            const std::string label =
                video.scene.empty() ? video.video_id : video.video_id + "   (scene " + video.scene + ")";
            if (ImGui::Selectable(label.c_str(), session_.selected_video() == i)) {
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
            draw_preview(videos[*selected]);
            draw_details(videos[*selected]);
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
        for (std::size_t f = 0; f < forms.size(); ++f) {
            if (ImGui::BeginTabItem(forms[f].title.c_str())) {
                draw_form(forms[f], f);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
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
            draw_choice(field);
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
        const float buttons = (browse ? 32.0F : 0.0F) + (field.suggestions.empty() ? 0.0F : 32.0F);
        ImGui::SetNextItemWidth(std::max(120.0F, ImGui::GetContentRegionAvail().x - buttons));
        const std::string hint = field.default_value.empty() ? std::string()
                                                             : "default: " + field.default_value;
        input_text("##value", field.value, ImGuiInputTextFlags_None, hint.c_str());
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
    }

    static void draw_choice(Field& field) {
        ImGui::SetNextItemWidth(200);
        if (ImGui::BeginCombo("##value", field.value.c_str())) {
            for (const std::string& choice : field.choices) {
                if (ImGui::Selectable(choice.c_str(), choice == field.value)) {
                    field.value = choice;
                }
            }
            ImGui::EndCombo();
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

    // ------------------------------------------------------------------ output

    void draw_log() {
        ImGui::Separator();
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
        forms[result->form].fields[result->field].value =
            inside ? relative.generic_string() : chosen.string();
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
    LauncherApp app(session, preview, gui, options.project_directory, options.settings,
                    options.scenes);
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
