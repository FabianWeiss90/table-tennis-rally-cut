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
constexpr std::string_view kPipeline = "pipeline";

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
    match_day_.reset();
    if (!workspace_.videos.empty()) {
        selected_ = 0;
    }
    rebuild_forms();
}

void LauncherSession::select_video(std::size_t index) {
    static_cast<void>(workspace_.videos.at(index)); // bounds check
    selected_ = index;
    match_day_.reset();
    std::vector<CommandForm> previous = std::move(forms_);
    rebuild_forms();
    for (std::size_t f = 0; f < forms_.size() && f < previous.size(); ++f) {
        if (forms_[f].name == kVideoIndependentForm && previous[f].name == forms_[f].name) {
            forms_[f] = std::move(previous[f]);
        }
    }
}

void LauncherSession::select_match_day(const std::string& match_day) {
    const auto first = std::ranges::find(workspace_.videos, match_day, &VideoEntry::match_day);
    if (first == workspace_.videos.end()) {
        throw std::invalid_argument(std::format("match day {} has no videos", match_day));
    }
    const auto index = static_cast<std::size_t>(first - workspace_.videos.begin());
    if (selected_ != index) {
        select_video(index);
    }
    match_day_ = match_day;
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
    if (match_day_ && (!selected_ || workspace_.videos[*selected_].match_day != *match_day_)) {
        match_day_.reset();
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
    if (field.flag == "--codec" || field.flag == "--resolution") {
        for (CommandForm& existing : forms_) {
            if (existing.name == form.name) {
                propose_bitrate(existing);
            }
        }
        return;
    }
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
        QueuedCommand command{video.original, video.video_id, form, unedited,
                              scope == BatchScope::All && form.name == kFeaturesForm};
        std::string reason;
        if (!prepare(command, reason)) {
            skipped.push_back(std::format("{} ({})", video.video_id, reason));
            continue;
        }
        queue_.push_back(std::move(command));
    }
    std::string header = std::format("{} for match day {}: {} to run", form.name,
                                     chosen.match_day, queue_.size());
    if (done > 0) {
        header += std::format(", {} already done", done);
    }
    header += skipped.empty() ? "\n" : "\nSkipped: " + joined(skipped) + "\n";
    start_batch(form.name, std::move(header));
}

void LauncherSession::run_pipeline(BatchScope scope) {
    if (running_) {
        throw std::logic_error("another command is still running");
    }
    if (!selected_) {
        throw std::logic_error("no match day chosen");
    }
    const VideoEntry& chosen = workspace_.videos[*selected_];
    const std::vector<CommandForm> proposed = forms_for(chosen, workspace_);
    queue_.clear();
    std::string plan;
    for (const VideoEntry& video : workspace_.videos) {
        if (video.match_day != chosen.match_day) {
            continue;
        }
        const std::vector<std::string> steps = pipeline_for(pipeline_steps_, video, scope);
        plan += std::format("  {}: {}\n", video.video_id,
                            steps.empty() ? std::string("all done") : joined(steps));
        for (const std::string& step : steps) {
            queue_.push_back({video.original, std::format("{}: {}", video.video_id, step),
                              form_named(forms_, step), form_named(proposed, step),
                              scope == BatchScope::All && step == kFeaturesForm});
        }
    }
    start_batch(std::string(kPipeline),
                std::format("pipeline ({}) for match day {}: {} steps to run\n{}",
                            joined(pipeline_steps_), chosen.match_day, queue_.size(), plan));
}

void LauncherSession::start_batch(std::string name, std::string header) {
    log_ = std::move(header);
    batch_name_ = std::move(name);
    batch_total_ = queue_.size();
    batch_failed_.clear();
    batch_skipped_ = 0;
    if (queue_.empty()) {
        finish_batch();
        return;
    }
    start_next();
}

std::optional<Invocation> LauncherSession::prepare(const QueuedCommand& command,
                                                   std::string& skipped) const {
    const auto video = std::ranges::find(workspace_.videos, command.original,
                                         &VideoEntry::original);
    if (video == workspace_.videos.end()) {
        skipped = "no longer in the library";
        return std::nullopt;
    }
    if (skipped = missing_input(command.edited.name, *video); !skipped.empty()) {
        return std::nullopt;
    }
    CommandForm own = carry_over_edits(command.edited, command.unedited,
                                       form_named(forms_for(*video, workspace_),
                                                  command.edited.name));
    if (command.recompute) {
        if (Field* force = own.field(std::string(kRecomputeFlag))) {
            force->value = "1";
        }
    }
    if (const auto missing = own.missing(); !missing.empty()) {
        skipped = "fill in " + missing.front();
        return std::nullopt;
    }
    return invocation(own);
}

void LauncherSession::start_next() {
    while (!queue_.empty()) {
        QueuedCommand next = std::move(queue_.front());
        queue_.pop_front();
        const std::size_t number = batch_total_ - queue_.size();
        std::string skipped;
        const std::optional<Invocation> call = prepare(next, skipped);
        if (!call) {
            append_log(std::format("\n[{}/{}] {} skipped: {}\n", number, batch_total_,
                                   next.label, skipped));
            ++batch_skipped_;
            continue;
        }
        append_log(std::format("\n[{}/{}] {}\n$ {}\n", number, batch_total_, next.label,
                               command_line(call->program, call->arguments)));
        try {
            runner_.start(*call);
        } catch (const std::exception& error) {
            append_log(std::format("{}\n", error.what()));
            batch_failed_.push_back(next.label);
            drop_rest_of(next.original);
            continue;
        }
        running_ = true;
        running_name_ = next.label;
        running_original_ = next.original;
        status_ = std::format("{} {}/{}: {} is running...", batch_name_, number, batch_total_,
                              next.label);
        return;
    }
    finish_batch();
}

void LauncherSession::finish_batch() {
    const std::size_t succeeded = batch_total_ - batch_failed_.size() - batch_skipped_;
    const char* unit = batch_name_ == kPipeline ? "steps" : "videos";
    status_ = batch_failed_.empty()
                  ? std::format("{} finished for {} {}", batch_name_, succeeded, unit)
                  : std::format("{} failed for {} of {} {}: {}", batch_name_,
                                batch_failed_.size(), batch_total_, unit, joined(batch_failed_));
    if (batch_skipped_ > 0) {
        status_ += std::format(" ({} skipped)", batch_skipped_);
    }
    append_log("\n" + status_ + "\n");
    batch_name_.clear();
    rescan();
}

void LauncherSession::cancel() {
    if (running_) {
        for (const QueuedCommand& dropped : queue_) {
            batch_failed_.push_back(dropped.label + " (cancelled)");
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
            drop_rest_of(running_original_);
        }
        rescan(); // shows each finished video at once
        start_next();
        return;
    }
    status_ = code == 0 ? running_name_ + " finished"
                        : std::format("{} failed (exit code {})", running_name_, code);
    rescan();
}

void LauncherSession::drop_rest_of(const std::string& original) {
    // Later steps of the video would work on a missing or stale result
    std::vector<std::string> dropped;
    std::erase_if(queue_, [&](const QueuedCommand& command) {
        if (command.original != original) {
            return false;
        }
        dropped.push_back(command.label);
        return true;
    });
    if (!dropped.empty()) {
        append_log(std::format("Not run: {}\n", joined(dropped)));
        batch_total_ -= dropped.size();
    }
}

void LauncherSession::append_log(const std::string& text) {
    log_ += text;
    if (log_.size() > kMaxLogBytes) {
        log_.erase(0, log_.size() - kMaxLogBytes);
    }
}

} // namespace ttrally::launcher
