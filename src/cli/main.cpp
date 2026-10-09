// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/align_command.hpp"
#ifdef TTRALLY_WITH_GUI
#include "cli/annotate_command.hpp"
#endif
#ifdef TTRALLY_WITH_ORT
#include "cli/detect_command.hpp"
#include "cli/features_command.hpp"
#endif
#include "cli/devices_command.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "shared/kernel/version.hpp"

#include <CLI/CLI.hpp>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    CLI::App app{"ttrally - rally detection for table tennis videos"};
    app.set_version_flag("--version", std::string{ttrally::version()});
    app.require_subcommand(1);
    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Show FFmpeg diagnostics");

    ttrally::cli::AlignCommand align(app);
#ifdef TTRALLY_WITH_GUI
    ttrally::cli::AnnotateCommand annotate(app);
#endif
#ifdef TTRALLY_WITH_ORT
    ttrally::cli::FeaturesCommand features(app);
    ttrally::cli::DetectCommand detect(app);
#endif
    ttrally::cli::DevicesCommand devices(app);

    CLI11_PARSE(app, argc, argv);
    ttrally::media::configure_ffmpeg_logging(verbose);

    try {
        if (align.selected()) {
            return align.run();
        }
#ifdef TTRALLY_WITH_GUI
        if (annotate.selected()) {
            return annotate.run();
        }
#endif
#ifdef TTRALLY_WITH_ORT
        if (features.selected()) {
            return features.run();
        }
        if (detect.selected()) {
            return detect.run();
        }
#endif
        if (devices.selected()) {
            return devices.run();
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
