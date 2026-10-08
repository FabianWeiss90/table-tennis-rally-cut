// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/infrastructure/csv_annotation_repository.hpp"

#include "shared/io/csv.hpp"
#include "shared/io/file_cache.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace ttrally::annotation {

namespace {

const io::CsvRow kHeader{"video_id", "rally_id",           "start_frame", "end_frame",
                         "fps",      "serve_contact_frame", "flags",       "notes"};
constexpr std::string_view kAbortedToss = "aborted_toss";
constexpr std::string_view kLet = "let";
constexpr std::string_view kIgnore = "ignore";

enum Column { VideoId, RallyId, StartFrame, EndFrame, Fps, ServeContact, Flags, Notes };

std::int64_t parse_frame(const std::string& text, const std::filesystem::path& file) {
    try {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used);
        if (used == text.size()) {
            return value;
        }
    } catch (const std::exception&) {
    }
    throw std::runtime_error(std::format("{}: invalid frame number \"{}\"", file.string(), text));
}

bool has_flag(const std::string& flags, std::string_view flag) {
    std::size_t start = 0;
    while (start <= flags.size()) {
        const std::size_t end = std::min(flags.find(';', start), flags.size());
        if (std::string_view(flags).substr(start, end - start) == flag) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

RallyLabel parse_row(const io::CsvRow& row, const std::filesystem::path& file) {
    if (row.size() != kHeader.size()) {
        throw std::runtime_error(std::format("{}: expected {} columns, found {}", file.string(),
                                             kHeader.size(), row.size()));
    }
    RallyLabel rally;
    rally.start_frame = parse_frame(row[StartFrame], file);
    rally.end_frame = parse_frame(row[EndFrame], file);
    if (!row[ServeContact].empty()) {
        rally.serve_contact_frame = parse_frame(row[ServeContact], file);
    }
    rally.aborted_toss = has_flag(row[Flags], kAbortedToss);
    rally.let = has_flag(row[Flags], kLet);
    rally.notes = row[Notes];
    return rally;
}

/// Flags column: names separated by ";".
std::string flags_of(const RallyLabel& rally) {
    std::string flags;
    for (const auto& [set, name] : {std::pair{rally.aborted_toss, kAbortedToss},
                                    std::pair{rally.let, kLet}}) {
        if (set) {
            flags += (flags.empty() ? "" : ";") + std::string(name);
        }
    }
    return flags;
}

} // namespace

std::string format_label_fps(Rational fps) {
    std::string text = std::format("{:.2f}", fps.value());
    while (text.back() == '0') {
        text.pop_back();
    }
    if (text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::filesystem::path CsvAnnotationRepository::file_for(const std::string& video_id) const {
    return directory_ / (video_id + ".csv");
}

StoredLabels CsvAnnotationRepository::load(const std::string& video_id) {
    const auto file = file_for(video_id);
    if (!std::filesystem::exists(file)) {
        return {};
    }
    const auto rows = io::read_csv(file);
    if (rows.empty() || rows.front() != kHeader) {
        throw std::runtime_error(file.string() + ": unexpected header");
    }
    StoredLabels labels;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].size() == 1 && rows[i].front().empty()) {
            continue; // trailing empty line
        }
        if (rows[i][VideoId] != video_id) {
            continue;
        }
        RallyLabel parsed = parse_row(rows[i], file);
        if (has_flag(rows[i][Flags], kIgnore)) {
            labels.ignored.push_back({parsed.start_frame, parsed.end_frame, parsed.notes});
        } else {
            labels.rallies.push_back(std::move(parsed));
        }
    }
    return labels;
}

void CsvAnnotationRepository::save(const AnnotationSheet& sheet) {
    const std::string fps = format_label_fps(sheet.fps());
    std::vector<std::pair<std::int64_t, io::CsvRow>> rows; // by start frame
    int id = 0;
    for (const RallyLabel& rally : sheet.rallies()) {
        rows.push_back({rally.start_frame,
                        {sheet.video_id(), std::to_string(++id), std::to_string(rally.start_frame),
                         std::to_string(rally.end_frame), fps,
                         rally.serve_contact_frame ? std::to_string(*rally.serve_contact_frame)
                                                   : "",
                         flags_of(rally), rally.notes}});
    }
    for (const IgnoredSection& section : sheet.ignored_sections()) {
        rows.push_back({section.start_frame,
                        {sheet.video_id(), "", std::to_string(section.start_frame),
                         std::to_string(section.end_frame), fps, "", std::string(kIgnore),
                         section.notes}});
    }
    std::ranges::stable_sort(rows, {}, &std::pair<std::int64_t, io::CsvRow>::first);
    std::vector<io::CsvRow> csv_rows;
    csv_rows.reserve(rows.size());
    for (auto& [start, row] : rows) {
        csv_rows.push_back(std::move(row));
    }
    const auto file = file_for(sheet.video_id());
    auto temporary = file;
    temporary += ".tmp";
    std::filesystem::create_directories(file.parent_path());
    io::write_csv(temporary, kHeader, csv_rows);
    std::filesystem::rename(temporary, file); // never leave a half-written label file
}

} // namespace ttrally::annotation
