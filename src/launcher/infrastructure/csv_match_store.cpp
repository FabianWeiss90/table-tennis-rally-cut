// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/csv_match_store.hpp"

#include "media/infrastructure/part_list.hpp"
#include "shared/io/csv.hpp"

#include <algorithm>

namespace ttrally::launcher {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kFileName = "matches.csv";

} // namespace

std::vector<Match> CsvMatchStore::load(const fs::path& work_dir) {
    std::vector<Match> matches;
    const fs::path file = work_dir / kFileName;
    if (!fs::exists(file)) {
        return matches;
    }
    const auto rows = io::read_csv(file);
    for (std::size_t r = 1; r < rows.size(); ++r) { // row 0 is the header
        if (rows[r].size() < 2 || rows[r][0].empty() || rows[r][1].empty()) {
            continue;
        }
        const std::string& id = rows[r][0];
        auto match = std::ranges::find(matches, id, &Match::id);
        if (match == matches.end()) {
            matches.push_back({id, {}});
            match = std::prev(matches.end());
        }
        match->parts.push_back(rows[r][1]);
    }
    return matches;
}

void CsvMatchStore::save(const fs::path& work_dir, const std::vector<Match>& matches) {
    const fs::path day = fs::absolute(work_dir).lexically_normal().parent_path();
    const fs::path lists = work_dir / kMatchesFolder;
    std::vector<io::CsvRow> rows;
    for (const Match& match : matches) {
        std::vector<fs::path> parts;
        for (const std::string& part : match.parts) {
            rows.push_back({match.id, part});
            parts.push_back(day / part);
        }
        media::write_part_list(lists / part_list_name(match.id), parts);
    }
    fs::create_directories(work_dir);
    io::write_csv(work_dir / kFileName, {"match_id", "part"}, rows);
    // Lists of matches split again
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(lists, error)) {
        const std::string name = entry.path().filename().string();
        const bool kept = std::ranges::any_of(matches, [&](const Match& match) {
            return name == part_list_name(match.id) || name == pre_cut_list_name(match.id);
        });
        if (!kept && entry.path().extension() == ".ffconcat") {
            fs::remove(entry.path(), error);
        }
    }
}

} // namespace ttrally::launcher
