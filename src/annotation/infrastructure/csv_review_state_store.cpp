// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/infrastructure/csv_review_state_store.hpp"

#include "shared/io/csv.hpp"

#include <stdexcept>

namespace ttrally::annotation {

namespace {

const io::CsvRow kHeader{"kind", "id", "status"};

} // namespace

std::filesystem::path CsvReviewStateStore::file_for(const std::string& video_id) const {
    return directory_ / (video_id + ".review.csv");
}

ReviewStatusMap CsvReviewStateStore::load(const std::string& video_id) {
    const auto file = file_for(video_id);
    ReviewStatusMap statuses;
    if (!std::filesystem::exists(file)) {
        return statuses;
    }
    const auto rows = io::read_csv(file);
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() != kHeader.size()) {
            continue;
        }
        const auto kind = parse_review_kind(row[0]);
        const auto status = parse_review_status(row[2]);
        if (!kind || !status) {
            throw std::runtime_error(file.string() + ": invalid row " + std::to_string(r + 1));
        }
        statuses[{*kind, std::stoi(row[1])}] = *status;
    }
    return statuses;
}

void CsvReviewStateStore::save(const std::string& video_id, const ReviewStatusMap& statuses) {
    std::vector<io::CsvRow> rows;
    for (const auto& [key, status] : statuses) {
        rows.push_back({std::string(to_string(key.first)), std::to_string(key.second),
                        std::string(to_string(status))});
    }
    const auto file = file_for(video_id);
    std::filesystem::create_directories(file.parent_path());
    io::write_csv(file, kHeader, rows);
}

} // namespace ttrally::annotation
