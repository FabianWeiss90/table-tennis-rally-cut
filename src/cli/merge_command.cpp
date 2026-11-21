// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cli/merge_command.hpp"

#include "cli/stream_progress_reporter.hpp"
#include "cutting/application/merge_videos.hpp"
#include "cutting/infrastructure/ffmpeg_video_joiner.hpp"
#include "shared/io/formatting.hpp"

#include <format>
#include <iostream>

namespace ttrally::cli {

MergeCommand::MergeCommand(CLI::App& app)
    : command_(app.add_subcommand(
          "merge", "Append the parts of a game recorded in several files into one new video, "
                   "without re-encoding (the parts stay as they are)")) {
    command_->add_option("PARTS", parts_, "The parts, in the order they were recorded")
        ->required()
        ->expected(2, -1)
        ->check(CLI::ExistingFile);
    command_->add_option("--out", out_,
                         "The merged video (must not exist yet; its extension chooses the "
                         "container, e.g. .mp4)")
        ->required();
}

int MergeCommand::run() {
    cutting::FfmpegVideoJoiner joiner;
    StreamProgressReporter lines(std::cout);
    TimestampedProgressReporter progress(lines);
    cutting::MergeVideos merge(joiner, progress);
    const auto result = merge.execute({.parts = parts_, .output = out_});
    progress.report(std::format("Done in {} in total: {} parts, {} of video",
                                io::format_elapsed(result.seconds), result.parts,
                                io::format_clock(result.duration_s)));
    progress.report("Merged video: " + out_.string());
    return 0;
}

} // namespace ttrally::cli
