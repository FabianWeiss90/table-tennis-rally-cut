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
    const std::vector<io::CsvRow> rows = io::read_csv(file);
    for (std::size_t r = 1; r < rows.size(); ++r) { // row 0 is the header
        if (rows[r].size() >= 2) {
            settings[rows[r][0]] = rows[r][1];
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

using Settings = std::map<std::string, std::string>;

Rgb parse_color(const Settings& settings, const std::string& key, Rgb fallback) {
    const auto found = settings.find(key);
    return found == settings.end() ? fallback : parse_hex(found->second).value_or(fallback);
}

std::string value_of(const Settings& settings, const std::string& key) {
    const auto found = settings.find(key);
    return found == settings.end() ? std::string() : found->second;
}

/// The title stored under keys "<prefix>" (text), "<prefix>_shown", "_size", "_position",
/// "_color" and "_outline".
Title read_title(const Settings& settings, const std::string& prefix, TitlePosition fallback) {
    Title title;
    title.text = value_of(settings, prefix);
    title.shown = value_of(settings, prefix + "_shown") != "0";
    title.size = parse_size(value_of(settings, prefix + "_size"));
    title.position = parse_title_position(value_of(settings, prefix + "_position")).value_or(fallback);
    title.color = parse_color(settings, prefix + "_color", kWhite);
    title.outlined = value_of(settings, prefix + "_outlined") == "1";
    title.outline = parse_color(settings, prefix + "_outline", kBlack);
    return title;
}

void put_title(Settings& settings, const std::string& prefix, const Title& title) {
    settings[prefix] = title.text;
    settings[prefix + "_shown"] = yes_no(title.shown);
    settings[prefix + "_size"] = std::format("{:.3f}", title.size);
    settings[prefix + "_position"] = std::string(to_string(title.position));
    settings[prefix + "_color"] = to_hex(title.color);
    settings[prefix + "_outlined"] = yes_no(title.outlined);
    settings[prefix + "_outline"] = to_hex(title.outline);
}

void write_settings(const fs::path& file, const Settings& settings) {
    std::vector<io::CsvRow> rows;
    for (const auto& [key, value] : settings) {
        rows.push_back({key, value});
    }
    io::write_csv(file, {"key", "value"}, rows);
}

constexpr std::string_view kLayoutPrefix = "title"; ///< players' titles in the match day video
constexpr std::string_view kMatchPrefix = "match";

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
    const Settings settings = read_settings(settings_file);
    StoredHighlights stored;
    stored.video = (match_day() / fs::path(value_of(settings, "video"))).lexically_normal();
    stored.coupled = value_of(settings, "coupled") != "0";
    stored.title = read_title(settings, "title", TitlePosition::BottomCenter);

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
    Settings settings{{"video", (inside ? relative : video).generic_string()},
                      {"coupled", yes_no(highlights.coupled)}};
    put_title(settings, "title", highlights.title);
    write_settings(directory_ / (video_id + std::string(kSettingsSuffix)), settings);
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
    const Title title = read_title(read_settings(file), std::string(kLayoutPrefix),
                                   TitlePosition::BottomCenter);
    return {.size = title.size,
            .position = title.position,
            .color = title.color,
            .outlined = title.outlined,
            .outline = title.outline};
}

void CsvHighlightStore::save_match_day_layout(const TitleLayout& layout) {
    const fs::path file = directory_ / kMatchDayTitleFile;
    fs::create_directories(directory_);
    Settings settings = fs::exists(file) ? read_settings(file) : Settings{};
    Title title = layout.applied_to({});
    put_title(settings, std::string(kLayoutPrefix), title);
    // Only the layout belongs here, not a text
    const std::string prefix(kLayoutPrefix);
    settings.erase(prefix);
    settings.erase(prefix + "_shown");
    write_settings(file, settings);
}

std::optional<Title> CsvHighlightStore::match_title() {
    const fs::path file = directory_ / kMatchDayTitleFile;
    if (!fs::exists(file)) {
        return std::nullopt;
    }
    const Settings settings = read_settings(file);
    if (!settings.contains(std::string(kMatchPrefix))) {
        return std::nullopt;
    }
    return read_title(settings, std::string(kMatchPrefix), TitlePosition::TopCenter);
}

fs::path CsvHighlightStore::title_font() {
    const fs::path file = directory_ / kMatchDayTitleFile;
    return fs::exists(file) ? fs::path(value_of(read_settings(file), "font")) : fs::path();
}

void CsvHighlightStore::save_title_font(const fs::path& font) {
    const fs::path file = directory_ / kMatchDayTitleFile;
    fs::create_directories(directory_);
    Settings settings = fs::exists(file) ? read_settings(file) : Settings{};
    settings["font"] = font.empty() ? std::string() : fs::absolute(font).string();
    write_settings(file, settings);
}

void CsvHighlightStore::save_match_title(const Title& title) {
    const fs::path file = directory_ / kMatchDayTitleFile;
    fs::create_directories(directory_);
    Settings settings = fs::exists(file) ? read_settings(file) : Settings{};
    put_title(settings, std::string(kMatchPrefix), title);
    write_settings(file, settings);
}

} // namespace ttrally::highlights
