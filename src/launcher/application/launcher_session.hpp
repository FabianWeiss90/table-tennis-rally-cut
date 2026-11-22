// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "launcher/application/ports.hpp"
#include "launcher/domain/batch.hpp"
#include "launcher/domain/forms.hpp"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::launcher {

/// Application service behind the launcher window: the workspace and its videos, one set of
/// forms for the selected video, and the command currently running with its output.
class LauncherSession {
  public:
    /// `ttrally` is the program run for the tool's own commands (normally this executable); the
    /// commands run in the project directory (which holds weights/ and the training). A pre-cut
    /// video chosen in the align form is kept in `pre_cuts` (if given) for the video.
    LauncherSession(WorkspaceScanner& scanner, ProcessRunner& runner, std::string ttrally,
                    std::filesystem::path project_directory, std::filesystem::path library,
                    PreCutStore* pre_cuts = nullptr, MatchStore* matches = nullptr);

    [[nodiscard]] const WorkspaceSnapshot& workspace() const noexcept { return workspace_; }
    [[nodiscard]] const std::filesystem::path& library() const noexcept { return library_; }
    /// Switches to another library (not while a command runs) and scans it.
    void set_library(std::filesystem::path library);
    [[nodiscard]] std::optional<std::size_t> selected_video() const noexcept { return selected_; }
    /// Fills the forms in for the video (edits made for the previous video are dropped, except
    /// in the training form, which does not depend on the video).
    void select_video(std::size_t index);
    /// Chooses a whole match day: its batches, pipeline and highlights. The forms are filled in
    /// for its first video (selected_video() names it). Throws std::invalid_argument for a match
    /// day without videos.
    void select_match_day(const std::string& match_day);
    /// The match day chosen as a whole; nullopt while a single video is selected.
    [[nodiscard]] const std::optional<std::string>& selected_match_day() const noexcept {
        return match_day_;
    }
    [[nodiscard]] std::vector<CommandForm>& forms() noexcept { return forms_; }
    /// To be called when the user has changed a field of one of the forms: keeps the pre-cut
    /// video chosen in the align form (an empty value forgets it).
    void field_edited(const CommandForm& form, const Field& field);

    /// Makes videos of one match day (originals, in recording order) one match named `id` and
    /// rescans; the results the videos had stay with them, unused. Throws std::invalid_argument
    /// (fewer than two, an id that is invalid or taken, videos of other match days or already
    /// part of a match), std::logic_error (no match store, a command running) or
    /// std::runtime_error (a part cannot be read, the list cannot be written).
    void join_match(const std::vector<std::string>& originals, const std::string& id);
    /// Splits a match (a video with parts) into its parts again and rescans; its own results
    /// stay, unused, until it is joined again under its id.
    void split_match(const VideoEntry& match);

    /// Scans the workspace again; keeps the selection and the form values, updates the
    /// suggestions.
    void rescan();

    /// The program and arguments a form runs.
    [[nodiscard]] Invocation invocation(const CommandForm& form) const;
    /// Starts a form's command. Throws std::logic_error if a command is running or required
    /// fields are empty, std::runtime_error if it cannot be started.
    void run(const CommandForm& form);
    /// Runs a form's command (see supports_batch) for the videos of the selected video's match
    /// day, one after another, with the edits made to fields that do not depend on the video.
    /// Videos that lack an input (e.g. align without a pre-cut video) are skipped and listed in
    /// the log; a failed video does not stop the others. Throws std::logic_error if a command is
    /// running.
    void run_batch(const CommandForm& form, BatchScope scope);
    /// Runs the ticked steps of the pipeline (see pipeline_for) for every video of the selected
    /// video's match day: all steps of one video, then the next. Each step uses the settings of
    /// its form (as for a batch); a failed step skips the rest of its video, a step without its
    /// input only itself. Throws std::logic_error if a command is running.
    void run_pipeline(BatchScope scope);
    /// The steps the pipeline runs (default_pipeline() at first).
    [[nodiscard]] std::vector<std::string>& pipeline_steps() noexcept { return pipeline_steps_; }
    /// Stops the running command and the rest of a batch.
    void cancel();
    /// Collects new output; when the command has ended, notes the result and rescans.
    void poll();

    [[nodiscard]] bool running() const noexcept { return running_; }
    /// Output of the current or last command.
    [[nodiscard]] const std::string& log() const noexcept { return log_; }
    /// e.g. "cut finished", "detect failed (exit code 1)"; empty before the first command.
    [[nodiscard]] const std::string& status() const noexcept { return status_; }

  private:
    void rebuild_forms();
    void append_log(const std::string& text);
    void start_next();
    void finish_batch();

    /// A command of a batch or pipeline, filled in for its video only when it starts (an earlier
    /// step may change what the form proposes, e.g. the rallies cut uses).
    struct QueuedCommand {
        std::string original; ///< of the video
        std::string label;    ///< e.g. "match-a" or "match-a: detect"
        CommandForm edited;   ///< the form with the user's settings
        CommandForm unedited; ///< as the form was proposed for the video it was edited for
        bool recompute = false; ///< features: computed again even if current
    };

    /// The command line of a queued command; empty with the reason in `skipped` if it lacks
    /// an input now.
    [[nodiscard]] std::optional<Invocation> prepare(const QueuedCommand& command,
                                                    std::string& skipped) const;
    void start_batch(std::string name, std::string header);
    /// Rescans; the match day stays chosen as a whole if it was (its first video may have
    /// changed).
    void rescan_keeping_match_day(const std::string& day);
    /// Removes the queued commands of a video whose command failed.
    void drop_rest_of(const std::string& original);

    WorkspaceScanner& scanner_;
    ProcessRunner& runner_;
    PreCutStore* pre_cuts_;
    MatchStore* matches_;
    std::string ttrally_;
    std::filesystem::path project_directory_;
    std::filesystem::path library_;
    WorkspaceSnapshot workspace_;
    std::optional<std::size_t> selected_;
    std::optional<std::string> match_day_; ///< chosen as a whole
    std::vector<std::string> pipeline_steps_ = default_pipeline();
    std::vector<CommandForm> forms_;
    bool running_ = false;
    std::string running_name_;
    std::string running_original_; ///< video of the running batch command
    std::string log_;
    std::string status_;
    std::deque<QueuedCommand> queue_; ///< the rest of a batch
    std::string batch_name_;          ///< empty: no batch runs
    std::size_t batch_total_ = 0;
    std::vector<std::string> batch_failed_;
    std::size_t batch_skipped_ = 0; ///< commands that lacked an input when their turn came
};

} // namespace ttrally::launcher
