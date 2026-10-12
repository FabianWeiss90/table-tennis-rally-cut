// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/launcher_app.hpp"

#include "gui/frame_texture.hpp"
#include "gui/gui_context.hpp"
#include "shared/io/formatting.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <condition_variable>
#include <format>
#include <imgui.h>
#include <mutex>
#include <thread>
#include <optional>
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

/// A path chosen in a native file dialog, delivered on SDL's thread and applied on ours.
struct DialogResult {
    std::size_t form = 0;
    std::size_t field = 0;
    std::string path;
};

class LauncherApp {
  public:
    LauncherApp(launcher::LauncherSession& session, launcher::VideoPreview& preview,
                const GuiContext& gui, std::filesystem::path project)
        : session_(session), gui_(gui), project_(std::filesystem::absolute(project)),
          preview_worker_(preview), preview_texture_(gui.renderer()) {}

    void draw() {
        session_.poll();
        apply_dialog_result();
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

    void draw_videos() {
        ImGui::TextUnformatted("Videos in data/");
        ImGui::SameLine(kSidebarWidth - 90);
        if (ImGui::SmallButton("Rescan")) {
            session_.rescan();
        }
        ImGui::Separator();
        const auto& videos = session_.workspace().videos;
        if (videos.empty()) {
            ImGui::TextWrapped("No videos found. Put the originals (and their pre-cut versions) "
                               "into data/ and press Rescan.");
        }
        for (std::size_t i = 0; i < videos.size(); ++i) {
            const VideoEntry& video = videos[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(video.video_id.c_str(), session_.selected_video() == i)) {
                if (!session_.running()) {
                    session_.select_video(i);
                }
            }
            draw_status(video);
            ImGui::PopID();
            ImGui::Spacing();
        }
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

    static void draw_details(const VideoEntry& video) {
        ImGui::TextWrapped("Original: %s", video.original.c_str());
        if (video.cut_candidates.empty()) {
            ImGui::TextColored(kMuted, "No pre-cut version (needed by align)");
        }
        for (const std::string& cut : video.cut_candidates) {
            ImGui::TextWrapped("Pre-cut: %s", cut.c_str());
        }
        const char* labels = video.annotation == AnnotationProgress::Complete     ? "complete"
                             : video.annotation == AnnotationProgress::InProgress ? "in progress"
                                                                                   : "none";
        ImGui::Text("Labels: %s (%zu rallies)", labels, video.rallies);
        ImGui::TextWrapped("Features: %s", video.features_model.empty()
                                               ? "none"
                                               : video.features_model.c_str());
        for (const std::string& cut : video.cuts) {
            ImGui::TextWrapped("Cut: %s", cut.c_str());
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

    static void draw_multi_choice(Field& field) {
        if (field.choices.empty()) {
            ImGui::TextColored(kMuted, "none yet (complete labels and features are needed)");
            return;
        }
        std::vector<std::string> selected = lines_of(field.value);
        bool changed = false;
        ImGui::BeginGroup();
        for (const std::string& choice : field.choices) {
            bool on = std::ranges::find(selected, choice) != selected.end();
            if (ImGui::Checkbox(choice.c_str(), &on)) {
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
        const std::string start = (project_ / (field.value.empty() ? "data" : field.value))
                                      .parent_path()
                                      .string();
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

    launcher::LauncherSession& session_;
    const GuiContext& gui_;
    std::filesystem::path project_;
    std::string error_;
    std::size_t shown_log_size_ = 0;
    DialogResult pending_target_;
    std::mutex dialog_mutex_;
    std::optional<DialogResult> dialog_result_;
    PreviewWorker preview_worker_;
    FrameTexture preview_texture_;
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
    LauncherApp app(session, preview, gui, options.project_directory);
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
