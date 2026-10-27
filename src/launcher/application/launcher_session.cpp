// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/application/launcher_session.hpp"

#include "launcher/domain/batch.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace ttrally::launcher {

namespace {

constexpr std::size_t kMaxLogBytes = 1 << 20; ///< older output is dropped beyond this
constexpr std::string_view kVideoIndependentForm = "train"; ///< kept when another video is chosen
constexpr std::string_view kAlignForm = "align";
constexpr std::string_view kAnnotateForm = "annotate"; ///< depends on the pre-cut video
constexpr std::string_view kPreCutField = "Pre-cut video";
constexpr std::string_view kFeaturesForm = "features";
constexpr std::string_view kRecomputeFlag = "--force"; ///< features are kept unless forced

const CommandForm& form_named(const std::vector<CommandForm>& forms, std::string_view name) {
    const auto it = std::ranges::find(forms, name, &CommandForm::name);
    if (it == forms.end()) {
        throw std::logic_error(std::format("no form {}", name));
    }
    return *it;
}

std::string joined(const std::vector<std::string>& items) {
    std::string text;
    for (const std::string& item : items) {
        text += (text.empty() ? "" : ", ") + item;
    }
    return text;
}

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
                                 std::filesystem::path library, PreCutStore* pre_cuts)
    : scanner_(scanner), runner_(runner), pre_cuts_(pre_cuts), ttrally_(std::move(ttrally)),
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

void LauncherSession::field_edited(const CommandForm& form, const Field& field) {
    if (pre_cuts_ == nullptr || form.name != kAlignForm || field.label != kPreCutField ||
        !selected_) {
        return;
    }
    const VideoEntry& video = workspace_.videos[*selected_];
    try {
        auto stored = pre_cuts_->load(video.work_dir);
        if (field.value.empty()) {
            stored.erase(video.video_id);
        } else {
            stored[video.video_id] =
                std::filesystem::absolute(project_directory_ / field.value).lexically_normal();
        }
        pre_cuts_->save(video.work_dir, stored);
    } catch (const std::exception& error) {
        append_log(std::format("Could not keep the pre-cut video: {}\n", error.what()));
        return;
    }
    rescan();
    if (!selected_) {
        return;
    }
    // Annotate opens the segments instead of the whole video once there is a pre-cut video
    const auto fresh = forms_for(workspace_.videos[*selected_], workspace_);
    for (CommandForm& existing : forms_) {
        if (existing.name == kAnnotateForm) {
            existing = form_named(fresh, kAnnotateForm);
        }
    }
}

void LauncherSession::run_batch(const CommandForm& form, BatchScope scope) {
    if (running_) {
        throw std::logic_error("another command is still running");
    }
    if (!selected_ || !supports_batch(form.name)) {
        throw std::logic_error(std::format("{} cannot run for a whole match day", form.name));
    }
    const VideoEntry& chosen = workspace_.videos[*selected_];
    const CommandForm unedited = form_named(forms_for(chosen, workspace_), form.name);
    std::vector<std::string> skipped;
    std::size_t done = 0;
    queue_.clear();
    for (const VideoEntry& video : workspace_.videos) {
        if (video.match_day != chosen.match_day) {
            continue;
        }
        if (scope == BatchScope::Missing && step_done(form.name, video)) {
            ++done;
            continue;
        }
        std::string reason = missing_input(form.name, video);
        CommandForm own = carry_over_edits(
            form, unedited, form_named(forms_for(video, workspace_), form.name));
        if (scope == BatchScope::All && form.name == kFeaturesForm) {
            if (Field* force = own.field(std::string(kRecomputeFlag))) {
                force->value = "1";
            }
        }
        if (const auto missing = own.missing(); reason.empty() && !missing.empty()) {
            reason = "fill in " + missing.front();
        }
        if (!reason.empty()) {
            skipped.push_back(std::format("{} ({})", video.video_id, reason));
            continue;
        }
        queue_.push_back({video.video_id, invocation(own)});
    }
    log_ = std::format("{} for match day {}: {} to run", form.name, chosen.match_day,
                       queue_.size());
    if (done > 0) {
        log_ += std::format(", {} already done", done);
    }
    log_ += skipped.empty() ? "\n" : "\nSkipped: " + joined(skipped) + "\n";
    batch_name_ = form.name;
    batch_total_ = queue_.size();
    batch_failed_.clear();
    if (queue_.empty()) {
        finish_batch();
        return;
    }
    start_next();
}

void LauncherSession::start_next() {
    while (!queue_.empty()) {
        QueuedCommand next = std::move(queue_.front());
        queue_.pop_front();
        const std::size_t number = batch_total_ - queue_.size();
        append_log(std::format("\n[{}/{}] {}\n$ {}\n", number, batch_total_, next.video,
                               command_line(next.call.program, next.call.arguments)));
        try {
            runner_.start(next.call);
        } catch (const std::exception& error) {
            append_log(std::format("{}\n", error.what()));
            batch_failed_.push_back(next.video);
            continue;
        }
        running_ = true;
        running_name_ = next.video;
        status_ = std::format("{} {}/{}: {} is running...", batch_name_, number, batch_total_,
                              next.video);
        return;
    }
    finish_batch();
}

void LauncherSession::finish_batch() {
    const std::size_t succeeded = batch_total_ - batch_failed_.size();
    status_ = batch_failed_.empty()
                  ? std::format("{} finished for {} videos", batch_name_, succeeded)
                  : std::format("{} failed for {} of {} videos: {}", batch_name_,
                                batch_failed_.size(), batch_total_, joined(batch_failed_));
    append_log("\n" + status_ + "\n");
    batch_name_.clear();
    rescan();
}

void LauncherSession::cancel() {
    if (running_) {
        for (const QueuedCommand& dropped : queue_) {
            batch_failed_.push_back(dropped.video + " (cancelled)");
        }
        queue_.clear();
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
    if (!batch_name_.empty()) {
        if (code != 0) {
            batch_failed_.push_back(running_name_);
            append_log(std::format("{} failed (exit code {})\n", running_name_, code));
        }
        rescan(); // shows each finished video at once
        start_next();
        return;
    }
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
