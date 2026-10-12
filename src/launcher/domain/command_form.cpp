// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/domain/command_form.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace ttrally::launcher {

namespace {

/// Values of a multi-choice field, one per line.
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

std::string as_argument(const Field& field, const std::string& working_directory) {
    if (!field.project_path || working_directory.empty() ||
        std::filesystem::path(field.value).is_absolute()) {
        return field.value;
    }
    // e.g. "data/features" seen from "training" is "../data/features"
    return std::filesystem::path(field.value)
        .lexically_relative(working_directory)
        .generic_string();
}

} // namespace

std::vector<std::string> CommandForm::arguments() const {
    std::vector<std::string> args = fixed_arguments;
    for (const Field& field : fields) {
        if (field.flag.empty() && !field.value.empty()) {
            args.push_back(as_argument(field, working_directory));
        }
    }
    for (const Field& field : fields) {
        if (field.flag.empty() || field.value.empty() ||
            (!field.required && field.value == field.default_value)) {
            continue;
        }
        switch (field.kind) {
        case FieldKind::Flag:
            args.push_back(field.flag);
            break;
        case FieldKind::MultiChoice: {
            args.push_back(field.flag);
            const auto values = lines_of(field.value);
            args.insert(args.end(), values.begin(), values.end());
            break;
        }
        default:
            args.push_back(field.flag);
            args.push_back(as_argument(field, working_directory));
        }
    }
    return args;
}

std::vector<std::string> CommandForm::missing() const {
    std::vector<std::string> labels;
    for (const Field& field : fields) {
        if (field.required && field.value.empty()) {
            labels.push_back(field.label);
        }
    }
    return labels;
}

Field* CommandForm::field(const std::string& flag) {
    const auto it = std::ranges::find(fields, flag, &Field::flag);
    return it == fields.end() ? nullptr : &*it;
}

std::string command_line(const std::string& program, const std::vector<std::string>& arguments) {
    auto quoted = [](const std::string& text) {
        if (!text.empty() && text.find_first_of(" \t\"'$&|;<>()*?") == std::string::npos) {
            return text;
        }
        std::string result = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                result += '\\';
            }
            result += c;
        }
        return result + "\"";
    };
    std::string line = quoted(program);
    for (const std::string& argument : arguments) {
        line += " " + quoted(argument);
    }
    return line;
}

} // namespace ttrally::launcher
