// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <string>
#include <vector>

namespace ttrally::launcher {

enum class FieldKind {
    File,        ///< a path, with a browse button
    Directory,   ///< a directory
    Text,        ///< free text or a number
    Choice,      ///< one of `choices`
    MultiChoice, ///< several of `choices` (passed after one flag)
    Flag,        ///< an option without value, on or off
};

/// One argument of a command, with its current value and what to suggest.
struct Field {
    std::string flag;  ///< e.g. "--before"; empty for positional arguments
    std::string label; ///< shown in the form
    FieldKind kind = FieldKind::Text;
    std::string value;         ///< current value; for flags "1" or empty; multi: one per line
    std::string default_value; ///< what the command uses without the option (not passed then)
    std::vector<std::string> suggestions; ///< offered in a drop-down list
    std::vector<std::string> choices;     ///< for Choice and MultiChoice
    bool required = false;
    bool advanced = false; ///< shown folded away under "Advanced"
    bool project_path = false; ///< a path relative to the project, also when run elsewhere
    std::string help;
};

/// What the launcher starts for a form.
enum class Program {
    Ttrally, ///< this tool
    Uv,      ///< the Python environment manager, for the training
};

/// The form of one command: its fields and how they become a command line.
struct CommandForm {
    std::string name;  ///< e.g. "cut"
    std::string title; ///< tab title
    std::string description;
    Program program = Program::Ttrally;
    std::string program_path; ///< for Program::Uv: the uv executable (editable)
    std::string working_directory; ///< relative to the project; empty: the project itself
    std::vector<std::string> fixed_arguments; ///< before the fields, e.g. the subcommand
    std::vector<Field> fields;

    /// Arguments after the program: fixed ones, positional fields, then options whose value
    /// differs from the command's default. Paths of project_path fields are made relative to
    /// the working directory.
    [[nodiscard]] std::vector<std::string> arguments() const;
    /// Labels of required fields without a value.
    [[nodiscard]] std::vector<std::string> missing() const;
    [[nodiscard]] Field* field(const std::string& flag);
};

/// A command line for display, with arguments quoted where needed.
[[nodiscard]] std::string command_line(const std::string& program,
                                       const std::vector<std::string>& arguments);

} // namespace ttrally::launcher
