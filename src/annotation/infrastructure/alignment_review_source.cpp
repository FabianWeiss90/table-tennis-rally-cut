// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/infrastructure/alignment_review_source.hpp"

#include "shared/io/csv.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>

namespace ttrally::annotation {

namespace {

/// Column positions looked up by name, so that additional columns do not matter.
class Columns {
  public:
    Columns(const io::CsvRow& header, const std::filesystem::path& file)
        : header_(header), file_(file) {}

    [[nodiscard]] std::size_t index(std::string_view name) const {
        const auto it = std::find(header_.begin(), header_.end(), name);
        if (it == header_.end()) {
            throw std::runtime_error(std::format("{}: column {} is missing", file_.string(), name));
        }
        return static_cast<std::size_t>(it - header_.begin());
    }

  private:
    const io::CsvRow& header_;
    const std::filesystem::path& file_;
};

std::vector<ReviewItem> read_items(const std::filesystem::path& file, ReviewKind kind,
                                   std::string_view id_column) {
    const auto rows = io::read_csv(file);
    if (rows.empty()) {
        throw std::runtime_error(file.string() + " is empty");
    }
    const Columns columns(rows.front(), file);
    const std::size_t id = columns.index(id_column);
    const std::size_t first = columns.index("orig_start_frame");
    const std::size_t last = columns.index("orig_end_frame");

    std::vector<ReviewItem> items;
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.size() < rows.front().size()) {
            continue; // trailing empty line
        }
        try {
            items.push_back({kind, std::stoi(row[id]), std::stoll(row[first]),
                             std::stoll(row[last]), ReviewStatus::Open});
        } catch (const std::exception&) {
            throw std::runtime_error(std::format("{}: invalid row {}", file.string(), r + 1));
        }
    }
    return items;
}

} // namespace

std::vector<ReviewItem> AlignmentReviewSource::load() {
    std::vector<ReviewItem> items = read_items(segments_csv_, ReviewKind::Candidate, "segment_id");
    if (std::filesystem::exists(gaps_csv_)) {
        auto gaps = read_items(gaps_csv_, ReviewKind::Gap, "gap_id");
        items.insert(items.end(), gaps.begin(), gaps.end());
    }
    return items;
}

} // namespace ttrally::annotation
