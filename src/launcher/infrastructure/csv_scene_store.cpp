// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "launcher/infrastructure/csv_scene_store.hpp"

#include "shared/io/csv.hpp"

namespace ttrally::launcher {

namespace {

constexpr std::string_view kFileName = "scenes.csv";

} // namespace

std::map<std::string, std::string> CsvSceneStore::load(const std::filesystem::path& work_dir) {
    const auto file = work_dir / kFileName;
    std::map<std::string, std::string> scenes;
    if (!std::filesystem::exists(file)) {
        return scenes;
    }
    const auto rows = io::read_csv(file);
    for (std::size_t r = 1; r < rows.size(); ++r) { // row 0 is the header
        if (rows[r].size() >= 2 && !rows[r][0].empty()) {
            scenes[rows[r][0]] = rows[r][1];
        }
    }
    return scenes;
}

void CsvSceneStore::save(const std::filesystem::path& work_dir,
                         const std::map<std::string, std::string>& scenes) {
    std::filesystem::create_directories(work_dir);
    std::vector<io::CsvRow> rows;
    for (const auto& [video_id, scene] : scenes) {
        rows.push_back({video_id, scene});
    }
    io::write_csv(work_dir / kFileName, {"video_id", "scene"}, rows);
}

} // namespace ttrally::launcher
