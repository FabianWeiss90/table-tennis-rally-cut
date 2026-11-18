// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/csv_highlight_store.hpp"

#include "shared/io/csv.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>
#include <stdexcept>

namespace ttrally::highlights {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kSettingsSuffix = ".settings.csv";
constexpr std::string_view kMatchDayTitleFile = "match-day-title.csv";

std::int64_t parse_frame(const std::string& text, const fs::path& file) {
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        throw std::runtime_error(std::format("{}: invalid frame '{}'", file.string(), text));
    }
    return value;
}

std::string yes_no(bool value) { return value ? "1" : "0"; }

std::map<std::string, std::string> read_settings(const fs::path& file) {
    std::map<std::string, std::string> settings;
    for (const io::CsvRow& row : io::read_csv(file)) {
        if (row.size() >= 2) {
            settings[row[0]] = row[1];
        }
    }
    return settings;
}

double parse_size(const std::string& text) {
    try {
        return std::clamp(std::stod(text), Title::kMinSize, Title::kMaxSize);
    } catch (const std::exception&) {
        return Title::kDefaultSize;
    }
}

} // namespace

CsvHighlightStore::CsvHighlightStore(fs::path directory) : directory_(std::move(directory)) {}

fs::path CsvHighlightStore::match_day() const {
    // <match day>/ttrally-data/highlights
    return fs::absolute(directory_).lexically_normal().parent_path().parent_path();
}

std::vector<std::string> CsvHighlightStore::games() {
    std::vector<std::string> ids;
    if (!fs::is_directory(directory_)) {
        return ids;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(directory_)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.ends_with(kSettingsSuffix)) {
            ids.push_back(name.substr(0, name.size() - kSettingsSuffix.size()));
        }
    }
    std::ranges::sort(ids);
    return ids;
}

std::optional<StoredHighlights> CsvHighlightStore::load(const std::string& video_id) {
    const fs::path settings_file = directory_ / (video_id + std::string(kSettingsSuffix));
    const fs::path rallies_file = directory_ / (video_id + ".csv");
    if (!fs::exists(settings_file) || !fs::exists(rallies_file)) {
        return std::nullopt;
    }
    std::map<std::string, std::string> settings = read_settings(settings_file);
    StoredHighlights stored;
    stored.video = (match_day() / fs::path(settings["video"])).lexically_normal();
    stored.coupled = settings["coupled"] != "0";
    stored.title.text = settings["title"];
    stored.title.shown = settings["title_shown"] != "0";
    stored.title.size = parse_size(settings["title_size"]);
    stored.title.position =
        parse_title_position(settings["title_position"]).value_or(TitlePosition::BottomCenter);

    const std::vector<io::CsvRow> rows = io::read_csv(rallies_file);
    for (std::size_t r = 1; r < rows.size(); ++r) { // row 0 is the header
        const io::CsvRow& row = rows[r];
        if (row.size() < 4) {
            continue;
        }
        stored.rallies.push_back({.start_frame = parse_frame(row[0], rallies_file),
                                  .end_frame = parse_frame(row[1], rallies_file),
                                  .game = row[2] == "1",
                                  .day = row[3] == "1"});
    }
    return stored;
}

void CsvHighlightStore::save(const std::string& video_id, const StoredHighlights& highlights) {
    fs::create_directories(directory_);
    const fs::path video = fs::absolute(highlights.video).lexically_normal();
    const fs::path relative = video.lexically_relative(match_day());
    const bool inside = !relative.empty() && *relative.begin() != "..";
    io::write_csv(directory_ / (video_id + std::string(kSettingsSuffix)), {"key", "value"},
                  {{"video", (inside ? relative : video).generic_string()},
                   {"coupled", yes_no(highlights.coupled)},
                   {"title", highlights.title.text},
                   {"title_shown", yes_no(highlights.title.shown)},
                   {"title_size", std::format("{:.3f}", highlights.title.size)},
                   {"title_position", std::string(to_string(highlights.title.position))}});
    std::vector<io::CsvRow> rows;
    for (const HighlightRally& rally : highlights.rallies) {
        rows.push_back({std::to_string(rally.start_frame), std::to_string(rally.end_frame),
                        yes_no(rally.game), yes_no(rally.day)});
    }
    io::write_csv(directory_ / (video_id + ".csv"), {"start_frame", "end_frame", "game", "day"},
                  rows);
}

TitleLayout CsvHighlightStore::match_day_layout() {
    const fs::path file = directory_ / kMatchDayTitleFile;
    if (!fs::exists(file)) {
        return {};
    }
    std::map<std::string, std::string> settings = read_settings(file);
    return {.size = parse_size(settings["title_size"]),
            .position = parse_title_position(settings["title_position"])
                            .value_or(TitlePosition::BottomCenter)};
}

void CsvHighlightStore::save_match_day_layout(const TitleLayout& layout) {
    fs::create_directories(directory_);
    io::write_csv(directory_ / kMatchDayTitleFile, {"key", "value"},
                  {{"title_size", std::format("{:.3f}", layout.size)},
                   {"title_position", std::string(to_string(layout.position))}});
}

} // namespace ttrally::highlights
