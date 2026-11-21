// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/batch.hpp"

#include <algorithm>
#include <array>

namespace ttrally::launcher {

namespace {

const Field* find(const CommandForm& form, const Field& like) {
    const auto it = std::ranges::find_if(form.fields, [&](const Field& field) {
        return field.flag == like.flag && field.label == like.label;
    });
    return it == form.fields.end() ? nullptr : &*it;
}

} // namespace

bool supports_batch(std::string_view command) {
    return std::ranges::find(kPipelineSteps, command) != kPipelineSteps.end();
}

bool step_done(std::string_view command, const VideoEntry& video) {
    if (command == "align") {
        return video.aligned;
    }
    if (command == "features") {
        return !video.features_model.empty();
    }
    if (command == "detect") {
        return video.detected;
    }
    if (command == "cut") {
        return !video.cuts.empty();
    }
    return false;
}

std::string missing_input(std::string_view command, const VideoEntry& video) {
    if (command == "align" && video.cut_candidates.empty()) {
        return "no pre-cut video";
    }
    if (command == "cut" && video.annotation == AnnotationProgress::None && !video.detected) {
        return "no labels or detections";
    }
    return {};
}

std::vector<std::string> default_pipeline() { return {"features", "detect", "cut"}; }

std::vector<std::string> pipeline_for(std::span<const std::string> steps, const VideoEntry& video,
                                      BatchScope scope) {
    std::vector<std::string> planned;
    for (const std::string_view step : kPipelineSteps) {
        if (std::ranges::find(steps, step) == steps.end()) {
            continue;
        }
        if (scope == BatchScope::All || !planned.empty() || !step_done(step, video)) {
            planned.emplace_back(step);
        }
    }
    return planned;
}

std::vector<bool> video_dependent_fields(std::string_view command, const VideoEntry& video,
                                         const WorkspaceSnapshot& workspace) {
    // Another video of the same match day: what differs follows from the video itself
    VideoEntry other = video;
    other.video_id += "\x01other";
    other.original += "\x01other";
    const auto named = [command](const std::vector<CommandForm>& forms) {
        const auto it = std::ranges::find(forms, command, &CommandForm::name);
        return it == forms.end() ? CommandForm{} : *it;
    };
    const CommandForm mine = named(forms_for(video, workspace));
    const CommandForm theirs = named(forms_for(other, workspace));
    std::vector<bool> dependent;
    for (const Field& field : mine.fields) {
        const Field* same = find(theirs, field);
        dependent.push_back(same == nullptr || same->value != field.value);
    }
    return dependent;
}

CommandForm carry_over_edits(const CommandForm& edited, const CommandForm& selected,
                             CommandForm other) {
    other.program_path = edited.program_path;
    for (Field& field : other.fields) {
        const Field* edit = find(edited, field);
        const Field* unedited = find(selected, field);
        if (edit != nullptr && unedited != nullptr && unedited->value == field.value) {
            field.value = edit->value;
        }
    }
    return other;
}

} // namespace ttrally::launcher
