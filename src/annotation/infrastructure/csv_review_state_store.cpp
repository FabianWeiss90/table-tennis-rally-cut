// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/infrastructure/csv_review_state_store.hpp"

#include "shared/io/csv.hpp"

#include <stdexcept>

namespace ttrally::annotation {

namespace {

const io::CsvRow kHeader{"kind", "id", "first_frame", "last_frame", "status"};
constexpr std::size_t kLegacyColumns = 3; // kind,id,status

enum Column { Kind, Id, FirstFrame, LastFrame, Status };

std::filesystem::path review_file(const std::filesystem::path& directory,
                                  const std::string& video_id) {
    return directory / (video_id + ".review.csv");
}

/// Reads both formats; the status is the last column in each.
ReviewStatusMap read_statuses(const std::filesystem::path& file) {
    ReviewStatusMap statuses;
    const auto rows = io::read_csv(file);
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() != kHeader.size() && row.size() != kLegacyColumns) {
            continue;
        }
        const auto kind = parse_review_kind(row[Kind]);
        const auto status = parse_review_status(row.back());
        if (!kind || !status) {
            throw std::runtime_error(file.string() + ": invalid row " + std::to_string(r + 1));
        }
        statuses[{*kind, std::stoi(row[Id])}] = *status;
    }
    return statuses;
}

} // namespace

std::filesystem::path CsvReviewStateStore::file_for(const std::string& video_id) const {
    return review_file(directory_, video_id);
}

ReviewStatusMap CsvReviewStateStore::load(const std::string& video_id) {
    const auto file = file_for(video_id);
    if (std::filesystem::exists(file)) {
        return read_statuses(file);
    }
    if (legacy_directory_) {
        const auto legacy = review_file(*legacy_directory_, video_id);
        if (std::filesystem::exists(legacy)) {
            return read_statuses(legacy);
        }
    }
    return {};
}

void CsvReviewStateStore::save(const std::string& video_id, const std::vector<ReviewItem>& items) {
    std::vector<io::CsvRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        rows.push_back({std::string(to_string(item.kind)), std::to_string(item.source_id),
                        std::to_string(item.first_frame), std::to_string(item.last_frame),
                        std::string(to_string(item.status))});
    }
    const auto file = file_for(video_id);
    std::filesystem::create_directories(file.parent_path());
    io::write_csv(file, kHeader, rows);
}

} // namespace ttrally::annotation
