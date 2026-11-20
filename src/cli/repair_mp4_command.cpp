// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/repair_mp4_command.hpp"

#include "cutting/infrastructure/mp4_repair.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <iostream>

namespace ttrally::cli {

namespace {

/// The MP4 files among the paths; folders are searched (not their subfolders).
std::vector<std::filesystem::path> mp4_files(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::filesystem::path> files;
    const auto is_mp4 = [](const std::filesystem::path& file) {
        std::string extension = file.extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".mp4";
    };
    for (const auto& path : paths) {
        if (!std::filesystem::is_directory(path)) {
            files.push_back(path);
            continue;
        }
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file() && is_mp4(entry.path())) {
                found.push_back(entry.path());
            }
        }
        std::ranges::sort(found);
        files.insert(files.end(), found.begin(), found.end());
    }
    return files;
}

std::string describe(const cutting::Mp4Check& check) {
    if (!check.h26x) {
        return "not H.264/H.265, nothing to check";
    }
    if (check.fine()) {
        return "fine";
    }
    std::string problems;
    if (!check.header_usable()) {
        problems = check.stream_sets.empty()
                       ? "no parameter sets anywhere (cannot be repaired)"
                       : "the header differs from the stream";
    }
    if (check.codec == cutting::NalCodec::H265 && !check.tagged_hvc1) {
        problems += std::string(problems.empty() ? "" : "; ") + "tagged hev1, not hvc1";
    }
    return problems;
}

} // namespace

RepairMp4Command::RepairMp4Command(CLI::App& app)
    : command_(app.add_subcommand(
          "repair-mp4", "Repair the video header of MP4 files written by earlier versions so "
                        "that Apple devices play them (no re-encoding; the files are replaced)")) {
    command_->add_option("PATHS", paths_, "MP4 files, or folders whose MP4 files are repaired")
        ->required()
        ->check(CLI::ExistingPath);
    command_->add_flag("--check", check_only_, "Only report what is wrong, change nothing");
}

int RepairMp4Command::run() {
    int failed = 0;
    for (const auto& file : mp4_files(paths_)) {
        try {
            if (check_only_) {
                std::cout << std::format("{}: {}\n", file.string(),
                                         describe(cutting::check_mp4(file)));
                continue;
            }
            switch (cutting::repair_mp4(file)) {
            case cutting::RepairOutcome::Repaired:
                std::cout << file.string() << ": repaired\n";
                break;
            case cutting::RepairOutcome::AlreadyFine:
                std::cout << file.string() << ": fine, unchanged\n";
                break;
            case cutting::RepairOutcome::NotRepairable:
                std::cout << file.string() << ": cannot be repaired ("
                          << describe(cutting::check_mp4(file)) << ")\n";
                ++failed;
                break;
            }
        } catch (const std::exception& error) {
            std::cout << file.string() << ": error: " << error.what() << '\n';
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}

} // namespace ttrally::cli
