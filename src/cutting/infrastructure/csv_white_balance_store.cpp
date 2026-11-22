// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/csv_white_balance_store.hpp"

#include "media/infrastructure/part_list.hpp"
#include "shared/io/csv.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>
#include <string>

namespace ttrally::cutting {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kAreasFile = "white-balance-areas.csv";
constexpr std::array<std::pair<WhiteBalanceChoice, std::string_view>, 3> kChoiceNames{{
    {WhiteBalanceChoice::Unchanged, "unchanged"},
    {WhiteBalanceChoice::Neutral, "neutral"},
    {WhiteBalanceChoice::Like, "like"},
}};

std::string_view name_of(WhiteBalanceChoice choice) {
    for (const auto& [value, name] : kChoiceNames) {
        if (value == choice) {
            return name;
        }
    }
    return "unchanged";
}

WhiteBalanceChoice choice_named(std::string_view name) {
    for (const auto& [value, known] : kChoiceNames) {
        if (known == name) {
            return value;
        }
    }
    return WhiteBalanceChoice::Unchanged;
}

double number(const std::string& text, double fallback) {
    try {
        return text.empty() ? fallback : std::stod(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string written(double value) { return std::format("{:.6f}", value); }

} // namespace

WhiteBalanceSettings CsvWhiteBalanceStore::load(const fs::path& work_dir) {
    WhiteBalanceSettings settings;
    if (const fs::path file = work_dir / kAreasFile; fs::exists(file)) {
        const auto rows = io::read_csv(file);
        for (std::size_t r = 1; r < rows.size(); ++r) { // row 0 is the header
            const auto& row = rows[r];
            if (row.size() >= 5 && !row[0].empty()) {
                settings.areas.emplace_back(row[0], RelativeRect{number(row[1], 0),
                                                                 number(row[2], 0),
                                                                 number(row[3], 0),
                                                                 number(row[4], 0)});
            }
        }
    }
    if (const fs::path file = work_dir / kWhiteBalanceFile; fs::exists(file)) {
        const auto rows = io::read_csv(file);
        for (std::size_t r = 1; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (row.size() >= 6 && !row[0].empty()) {
                settings.files.push_back({row[0], choice_named(row[1]), row[2],
                                          {number(row[3], 1), number(row[4], 1),
                                           number(row[5], 1)}});
            }
        }
    }
    return settings;
}

void CsvWhiteBalanceStore::save(const fs::path& work_dir, const WhiteBalanceSettings& settings) {
    fs::create_directories(work_dir);
    std::vector<io::CsvRow> areas;
    for (const auto& [scene, area] : settings.areas) {
        areas.push_back({scene, written(area.left), written(area.top), written(area.right),
                         written(area.bottom)});
    }
    io::write_csv(work_dir / kAreasFile, {"scene", "left", "top", "right", "bottom"}, areas);
    std::vector<io::CsvRow> files;
    for (const FileWhiteBalance& file : settings.files) {
        files.push_back({file.file, std::string(name_of(file.choice)), file.like,
                         written(file.gains.red), written(file.gains.green),
                         written(file.gains.blue)});
    }
    io::write_csv(work_dir / kWhiteBalanceFile, {"file", "choice", "like", "red", "green", "blue"},
                  files);
}

std::vector<ColorSpan> color_spans(const fs::path& video, const fs::path& white_balance) {
    if (white_balance.empty() || !fs::exists(white_balance)) {
        return {};
    }
    const WhiteBalanceSettings settings = CsvWhiteBalanceStore().load(white_balance.parent_path());
    std::vector<ColorSpan> spans;
    for (const media::PartSpan& part : media::part_spans(video)) {
        const std::string name = part.file.filename().string();
        const auto file = std::ranges::find(settings.files, name, &FileWhiteBalance::file);
        if (file != settings.files.end() && !file->gains.identity()) {
            spans.push_back({part.start_s, part.end_s, file->gains});
        }
    }
    return spans;
}

} // namespace ttrally::cutting
