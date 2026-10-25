// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/application/launcher_session.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace ttrally::launcher {

namespace {

constexpr std::size_t kMaxLogBytes = 1 << 20; ///< older output is dropped beyond this
constexpr std::string_view kVideoIndependentForm = "train"; ///< kept when another video is chosen

/// Drops ticked choices that are no longer offered.
void drop_unknown_choices(Field& field) {
    std::string kept;
    std::size_t begin = 0;
    while (begin < field.value.size()) {
        const std::size_t end = std::min(field.value.find('\n', begin), field.value.size());
        const std::string choice = field.value.substr(begin, end - begin);
        if (std::ranges::find(field.choices, choice) != field.choices.end()) {
            kept += choice + "\n";
        }
        begin = end + 1;
    }
    field.value = kept;
}

/// Takes the suggestions and choices of `fresh` for fields the user may have edited.
void update_suggestions(std::vector<CommandForm>& forms, const std::vector<CommandForm>& fresh) {
    for (std::size_t f = 0; f < forms.size() && f < fresh.size(); ++f) {
        for (Field& field : forms[f].fields) {
            const auto match = std::ranges::find_if(fresh[f].fields, [&](const Field& other) {
                return other.flag == field.flag && other.label == field.label;
            });
            if (match != fresh[f].fields.end()) {
                field.suggestions = match->suggestions;
                field.choices = match->choices;
                if (field.kind == FieldKind::MultiChoice) {
                    drop_unknown_choices(field);
                }
            }
        }
    }
}

} // namespace

LauncherSession::LauncherSession(WorkspaceScanner& scanner, ProcessRunner& runner,
                                 std::string ttrally, std::filesystem::path project_directory,
                                 std::filesystem::path library)
    : scanner_(scanner), runner_(runner), ttrally_(std::move(ttrally)),
      project_directory_(std::move(project_directory)), library_(std::move(library)),
      workspace_(scanner_.scan(library_)) {
    if (!workspace_.videos.empty()) {
        selected_ = 0;
    }
    rebuild_forms();
}

void LauncherSession::set_library(std::filesystem::path library) {
    if (running_) {
        throw std::logic_error("cannot switch the library while a command runs");
    }
    library_ = std::move(library);
    workspace_ = scanner_.scan(library_);
    selected_.reset();
    if (!workspace_.videos.empty()) {
        selected_ = 0;
    }
    rebuild_forms();
}

void LauncherSession::select_video(std::size_t index) {
    static_cast<void>(workspace_.videos.at(index)); // bounds check
    selected_ = index;
    std::vector<CommandForm> previous = std::move(forms_);
    rebuild_forms();
    for (std::size_t f = 0; f < forms_.size() && f < previous.size(); ++f) {
        if (forms_[f].name == kVideoIndependentForm && previous[f].name == forms_[f].name) {
            forms_[f] = std::move(previous[f]);
        }
    }
}

void LauncherSession::rebuild_forms() {
    const VideoEntry none;
    forms_ = forms_for(selected_ ? workspace_.videos[*selected_] : none, workspace_);
}

void LauncherSession::rescan() {
    const std::string original =
        selected_ ? workspace_.videos[*selected_].original : std::string();
    workspace_ = scanner_.scan(library_);
    selected_.reset();
    for (std::size_t i = 0; i < workspace_.videos.size(); ++i) {
        if (workspace_.videos[i].original == original) {
            selected_ = i;
        }
    }
    if (!selected_ && !workspace_.videos.empty()) {
        selected_ = 0;
        rebuild_forms();
        return;
    }
    const VideoEntry none;
    update_suggestions(forms_, forms_for(selected_ ? workspace_.videos[*selected_] : none,
                                         workspace_));
}

Invocation LauncherSession::invocation(const CommandForm& form) const {
    const std::string program = form.program == Program::Uv
                                    ? (form.program_path.empty() ? "uv" : form.program_path)
                                    : ttrally_;
    return {program, form.arguments(),
            form.working_directory.empty() ? project_directory_
                                           : project_directory_ / form.working_directory};
}

void LauncherSession::run(const CommandForm& form) {
    if (running_) {
        throw std::logic_error("another command is still running");
    }
    if (const auto missing = form.missing(); !missing.empty()) {
        throw std::logic_error("fill in: " + missing.front());
    }
    const Invocation call = invocation(form);
    log_ = "$ " + command_line(call.program, call.arguments) + "\n";
    runner_.start(call);
    running_ = true;
    running_name_ = form.name;
    status_ = form.name + " is running...";
}

void LauncherSession::cancel() {
    if (running_) {
        runner_.cancel();
        status_ = running_name_ + " is being cancelled...";
    }
}

void LauncherSession::poll() {
    if (!running_) {
        return;
    }
    append_log(runner_.take_output());
    if (runner_.running()) {
        return;
    }
    append_log(runner_.take_output()); // the last words
    running_ = false;
    const int code = runner_.exit_code().value_or(-1);
    status_ = code == 0 ? running_name_ + " finished"
                        : std::format("{} failed (exit code {})", running_name_, code);
    rescan();
}

void LauncherSession::append_log(const std::string& text) {
    log_ += text;
    if (log_.size() > kMaxLogBytes) {
        log_.erase(0, log_.size() - kMaxLogBytes);
    }
}

} // namespace ttrally::launcher
