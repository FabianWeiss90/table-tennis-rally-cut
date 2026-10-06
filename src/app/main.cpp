// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "common/version.hpp"

#include <CLI/CLI.hpp>
#include <string>

int main(int argc, char** argv) {
    CLI::App app{"ttrally - rally detection for table tennis videos"};
    app.set_version_flag("--version", std::string{ttrally::version()});

    // Subcommands (align, devices) are added in Phase 1.

    CLI11_PARSE(app, argc, argv);
    return 0;
}
