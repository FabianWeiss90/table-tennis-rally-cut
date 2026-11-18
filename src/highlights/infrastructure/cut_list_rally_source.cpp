// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/cut_list_rally_source.hpp"

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "cutting/infrastructure/cut_list_choice.hpp"
#include "highlights/infrastructure/csv_highlight_store.hpp"
#include "shared/io/csv.hpp"

#include <algorithm>

namespace ttrally::highlights {

namespace {

constexpr std::string_view kFpsColumn = "fps";

/// The frame rate written in a label file (the same in every row).
double label_fps(const std::filesystem::path& file) {
    const std::vector<io::CsvRow> rows = io::read_csv(file);
    if (rows.size() < 2) {
        return 0.0;
    }
    const auto column = std::ranges::find(rows[0], kFpsColumn);
    const auto index = static_cast<std::size_t>(column - rows[0].begin());
    if (column == rows[0].end() || index >= rows[1].size()) {
        return 0.0;
    }
    try {
        return std::stod(rows[1][index]);
    } catch (const std::exception&) {
        return 0.0;
    }
}

} // namespace

CutListRallySource::CutListRallySource(std::filesystem::path work_dir)
    : work_dir_(std::move(work_dir)) {}

std::optional<RallyList> CutListRallySource::rallies(const std::string& video_id) {
    const cutting::CutListChoice choice =
        cutting::choose_cut_list(work_dir_ / "annotations", work_dir_ / "refined",
                                 work_dir_ / "detections", video_id);
    if (!std::filesystem::exists(choice.file)) {
        return std::nullopt;
    }
    const annotation::StoredLabels labels =
        annotation::CsvAnnotationRepository(choice.file.parent_path()).load(video_id);
    RallyList list{.rallies = {}, .fps = label_fps(choice.file), .source = choice.kind};
    for (const annotation::RallyLabel& rally : labels.rallies) {
        list.rallies.push_back({.start_frame = rally.start_frame,
                                .end_frame = rally.end_frame,
                                .game = false,
                                .day = false});
    }
    return list;
}

std::unique_ptr<HighlightStore>
FileMatchDayHighlights::store(const std::filesystem::path& work_dir) {
    return std::make_unique<CsvHighlightStore>(work_dir / "highlights");
}

std::unique_ptr<RallySource>
FileMatchDayHighlights::rallies(const std::filesystem::path& work_dir) {
    return std::make_unique<CutListRallySource>(work_dir);
}

} // namespace ttrally::highlights
