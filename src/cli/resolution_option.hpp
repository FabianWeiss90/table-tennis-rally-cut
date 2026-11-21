// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <CLI/CLI.hpp>
#include <string>

namespace ttrally::cli {

/// The `--resolution` option of the commands that write videos (cut, highlights): "original",
/// "2160p", "1440p" or "1080p".
inline CLI::Option* add_resolution_option(CLI::App& command, std::string& resolution) {
    return command
        .add_option("--resolution", resolution,
                    "Height of the written video: original, 2160p (4K), 1440p or 1080p (Full "
                    "HD); a smaller original is not enlarged")
        ->check(CLI::IsMember({"original", "2160p", "1440p", "1080p"}))
        ->capture_default_str();
}

/// The largest height for a resolution of the option; 0 for the original.
[[nodiscard]] inline int max_height_of(const std::string& resolution) {
    return resolution == "original" ? 0 : std::stoi(resolution); // "1080p" -> 1080
}

} // namespace ttrally::cli
