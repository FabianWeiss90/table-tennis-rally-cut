// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/batch.hpp"

#include <algorithm>
#include <array>

namespace ttrally::launcher {

namespace {

constexpr std::array<std::string_view, 4> kBatchCommands{"align", "features", "detect", "cut"};

const Field* find(const CommandForm& form, const Field& like) {
    const auto it = std::ranges::find_if(form.fields, [&](const Field& field) {
        return field.flag == like.flag && field.label == like.label;
    });
    return it == form.fields.end() ? nullptr : &*it;
}

} // namespace

bool supports_batch(std::string_view command) {
    return std::ranges::find(kBatchCommands, command) != kBatchCommands.end();
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
