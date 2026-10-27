// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/csv_pre_cut_store.hpp"

#include "shared/io/csv.hpp"

namespace ttrally::launcher {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kFileName = "pre-cuts.csv";

fs::path match_day(const fs::path& work_dir) {
    return fs::absolute(work_dir).lexically_normal().parent_path();
}

} // namespace

std::map<std::string, fs::path> CsvPreCutStore::load(const fs::path& work_dir) {
    const auto file = work_dir / kFileName;
    std::map<std::string, fs::path> pre_cuts;
    if (!fs::exists(file)) {
        return pre_cuts;
    }
    const auto rows = io::read_csv(file);
    for (std::size_t r = 1; r < rows.size(); ++r) { // row 0 is the header
        if (rows[r].size() >= 2 && !rows[r][0].empty() && !rows[r][1].empty()) {
            pre_cuts[rows[r][0]] = (match_day(work_dir) / fs::path(rows[r][1])).lexically_normal();
        }
    }
    return pre_cuts;
}

void CsvPreCutStore::save(const fs::path& work_dir,
                          const std::map<std::string, fs::path>& pre_cuts) {
    fs::create_directories(work_dir);
    const fs::path day = match_day(work_dir);
    std::vector<io::CsvRow> rows;
    for (const auto& [video_id, pre_cut] : pre_cuts) {
        const fs::path absolute = fs::absolute(pre_cut).lexically_normal();
        const fs::path relative = absolute.lexically_relative(day);
        const bool inside = !relative.empty() && *relative.begin() != "..";
        rows.push_back({video_id, (inside ? relative : absolute).generic_string()});
    }
    io::write_csv(work_dir / kFileName, {"video_id", "pre_cut"}, rows);
}

} // namespace ttrally::launcher
