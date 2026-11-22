// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/csv_white_balance_store.hpp"

#include "shared/io/csv.hpp"

#include <algorithm>
#include <map>
#include <format>
#include <stdexcept>
#include <string>

namespace ttrally::cutting {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kReferenceFile = "white-balance-reference.csv";

double number(const std::string& text, double fallback) {
    try {
        return text.empty() ? fallback : std::stod(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string written(double value) { return std::format("{:.6f}", value); }

/// Rows of a CSV file as column name -> value (the first row names the columns); empty if the
/// file does not exist.
std::vector<std::map<std::string, std::string>> named_rows(const fs::path& file) {
    std::vector<std::map<std::string, std::string>> rows;
    if (!fs::exists(file)) {
        return rows;
    }
    const auto csv = io::read_csv(file);
    for (std::size_t r = 1; r < csv.size(); ++r) {
        std::map<std::string, std::string> row;
        for (std::size_t c = 0; c < csv[r].size() && c < csv[0].size(); ++c) {
            row[csv[0][c]] = csv[r][c];
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string value(const std::map<std::string, std::string>& row, const std::string& column) {
    const auto found = row.find(column);
    return found == row.end() ? std::string() : found->second;
}

} // namespace

WhiteBalanceSettings CsvWhiteBalanceStore::load(const fs::path& work_dir) {
    WhiteBalanceSettings settings;
    for (const auto& row : named_rows(work_dir / kWhiteBalanceFile)) {
        const std::string file = value(row, "file");
        if (file.empty()) {
            continue;
        }
        FileWhiteBalance balance{file, std::nullopt};
        const RelativeRect area{number(value(row, "left"), 0), number(value(row, "top"), 0),
                                number(value(row, "right"), 0), number(value(row, "bottom"), 0)};
        if (!area.empty()) {
            balance.area = area;
        }
        settings.files.push_back(std::move(balance));
    }
    const auto reference = named_rows(work_dir / kReferenceFile);
    if (!reference.empty()) {
        settings.reference_video = value(reference.front(), "reference_video");
        settings.reference_start_s = number(value(reference.front(), "reference_start"), 0);
        settings.reference_end_s = number(value(reference.front(), "reference_end"), 0);
        settings.temperature = number(value(reference.front(), "temperature"), 0);
        settings.tint = number(value(reference.front(), "tint"), 0);
        settings.brightness = number(value(reference.front(), "brightness"), 0);
        settings.match_brightness = value(reference.front(), "match_brightness") == "1";
    }
    return settings;
}

void CsvWhiteBalanceStore::save(const fs::path& work_dir, const WhiteBalanceSettings& settings) {
    fs::create_directories(work_dir);
    std::vector<io::CsvRow> files;
    for (const FileWhiteBalance& file : settings.files) {
        const RelativeRect area = file.area.value_or(RelativeRect{});
        const bool has_area = file.area.has_value();
        files.push_back({file.file, has_area ? written(area.left) : "",
                         has_area ? written(area.top) : "", has_area ? written(area.right) : "",
                         has_area ? written(area.bottom) : ""});
    }
    io::write_csv(work_dir / kWhiteBalanceFile, {"file", "left", "top", "right", "bottom"}, files);
    io::write_csv(work_dir / kReferenceFile,
                  {"reference_video", "reference_start", "reference_end", "temperature", "tint",
                   "brightness", "match_brightness"},
                  {{settings.reference_video, written(settings.reference_start_s),
                    written(settings.reference_end_s), written(settings.temperature),
                    written(settings.tint), written(settings.brightness),
                    settings.match_brightness ? "1" : "0"}});
}


} // namespace ttrally::cutting
