// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "alignment/infrastructure/csv_alignment_writer.hpp"

#include "shared/io/csv.hpp"
#include "shared/io/formatting.hpp"

#include <format>

namespace ttrally::alignment {

namespace {

void create_parent_directory(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
}

} // namespace

std::filesystem::path gaps_path_for(const std::filesystem::path& segments_csv) {
    auto path = segments_csv;
    path.replace_filename(segments_csv.stem().string() + ".gaps.csv");
    return path;
}

void write_segments_csv(const std::filesystem::path& path, const AlignmentReport& report) {
    const std::string fps = io::format_fps(report.original_fps);
    std::vector<io::CsvRow> rows;
    for (const CandidateSegment& c : report.candidates) {
        rows.push_back({std::to_string(c.id), io::format_seconds(c.cut_start_s),
                        io::format_seconds(c.cut_end_s), io::format_seconds(c.orig_start_s),
                        io::format_seconds(c.orig_end_s), std::to_string(c.orig_start_frame),
                        std::to_string(c.orig_end_frame), fps, io::format_seconds(c.offset_s),
                        std::format("{:.3f}", c.confidence)});
    }
    create_parent_directory(path);
    io::write_csv(path,
                  {"segment_id", "cut_start_s", "cut_end_s", "orig_start_s", "orig_end_s",
                   "orig_start_frame", "orig_end_frame", "orig_fps", "offset_s", "confidence"},
                  rows);
}

void write_gaps_csv(const std::filesystem::path& path, const AlignmentReport& report) {
    std::vector<io::CsvRow> rows;
    for (const Gap& gap : report.gaps) {
        const std::string after =
            gap.after_candidate > 0 ? std::to_string(gap.after_candidate) : "";
        rows.push_back({std::to_string(gap.id), after, io::format_seconds(gap.orig_start_s),
                        io::format_seconds(gap.orig_end_s), std::to_string(gap.orig_start_frame),
                        std::to_string(gap.orig_end_frame), io::format_seconds(gap.duration_s())});
    }
    create_parent_directory(path);
    io::write_csv(path,
                  {"gap_id", "after_segment_id", "orig_start_s", "orig_end_s", "orig_start_frame",
                   "orig_end_frame", "duration_s"},
                  rows);
}

} // namespace ttrally::alignment
