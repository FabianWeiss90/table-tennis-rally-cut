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

double parse_length(const Settings& settings, const std::string& key, double fallback,
                    double max) {
    const std::string text = value_of(settings, key);
    try {
        return text.empty() ? fallback : std::clamp(std::stod(text), 0.0, max);
    } catch (const std::exception&) {
        return fallback;
    }
}

int parse_opacity(const Settings& settings, const std::string& key, int fallback) {
    const std::string text = value_of(settings, key);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error != std::errc() || end != text.data() + text.size()
               ? fallback
               : std::clamp(value, 0, kMaxOpacity);
}

std::uint32_t parse_seed(const std::string& text, std::uint32_t fallback) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error != std::errc() || end != text.data() + text.size() ? fallback : value;
}

std::string length_text(double value) { return std::format("{:.3f}", value); }

/// The decoration stored under keys "<prefix>_backdrop...", "_shadow..." and "_glow..."; the
/// defaults for missing keys (files from before decorations).
TitleDecoration read_decoration(const Settings& settings, const std::string& prefix) {
    const TitleDecoration defaults;
    TitleDecoration d;
    Backdrop& b = d.backdrop;
    const std::string bp = prefix + "_backdrop";
    b.shape = parse_backdrop_shape(value_of(settings, bp)).value_or(BackdropShape::None);
    b.svg = value_of(settings, bp + "_svg");
    b.color = parse_color(settings, bp + "_color", defaults.backdrop.color);
    b.opacity = parse_opacity(settings, bp + "_opacity", defaults.backdrop.opacity);
    b.per_line = value_of(settings, bp + "_per_line") == "1";
    b.seed = parse_seed(value_of(settings, bp + "_seed"), defaults.backdrop.seed);
    b.padding = parse_length(settings, bp + "_padding", defaults.backdrop.padding,
                             Backdrop::kMaxLength);
    b.corner =
        parse_length(settings, bp + "_corner", defaults.backdrop.corner, Backdrop::kMaxLength);
    b.softness = parse_length(settings, bp + "_softness", defaults.backdrop.softness,
                              Backdrop::kMaxLength);
    b.bordered = value_of(settings, bp + "_bordered") == "1";
    b.border = parse_color(settings, bp + "_border", defaults.backdrop.border);
    b.border_width = parse_length(settings, bp + "_border_width", defaults.backdrop.border_width,
                                  Backdrop::kMaxBorder);
    TitleShadow& s = d.shadow;
    const std::string sp = prefix + "_shadow";
    s.shown = value_of(settings, sp) == "1";
    s.color = parse_color(settings, sp + "_color", defaults.shadow.color);
    s.opacity = parse_opacity(settings, sp + "_opacity", defaults.shadow.opacity);
    s.offset =
        parse_length(settings, sp + "_offset", defaults.shadow.offset, TitleShadow::kMaxLength);
    s.blur = parse_length(settings, sp + "_blur", defaults.shadow.blur, TitleShadow::kMaxLength);
    TitleGlow& g = d.glow;
    const std::string gp = prefix + "_glow";
    g.shown = value_of(settings, gp) == "1";
    g.color = parse_color(settings, gp + "_color", defaults.glow.color);
    g.opacity = parse_opacity(settings, gp + "_opacity", defaults.glow.opacity);
    g.size = parse_length(settings, gp + "_size", defaults.glow.size, TitleGlow::kMaxSize);
    return d;
}

void put_decoration(Settings& settings, const std::string& prefix, const TitleDecoration& d) {
    const Backdrop& b = d.backdrop;
    const std::string bp = prefix + "_backdrop";
    settings[bp] = std::string(to_string(b.shape));
    settings[bp + "_svg"] = b.svg;
    settings[bp + "_color"] = to_hex(b.color);
    settings[bp + "_opacity"] = std::to_string(b.opacity);
    settings[bp + "_per_line"] = yes_no(b.per_line);
    settings[bp + "_seed"] = std::to_string(b.seed);
    settings[bp + "_padding"] = length_text(b.padding);
    settings[bp + "_corner"] = length_text(b.corner);
    settings[bp + "_softness"] = length_text(b.softness);
    settings[bp + "_bordered"] = yes_no(b.bordered);
    settings[bp + "_border"] = to_hex(b.border);
    settings[bp + "_border_width"] = length_text(b.border_width);
    const TitleShadow& s = d.shadow;
    const std::string sp = prefix + "_shadow";
    settings[sp] = yes_no(s.shown);
    settings[sp + "_color"] = to_hex(s.color);
    settings[sp + "_opacity"] = std::to_string(s.opacity);
    settings[sp + "_offset"] = length_text(s.offset);
    settings[sp + "_blur"] = length_text(s.blur);
    const TitleGlow& g = d.glow;
    const std::string gp = prefix + "_glow";
    settings[gp] = yes_no(g.shown);
    settings[gp + "_color"] = to_hex(g.color);
    settings[gp + "_opacity"] = std::to_string(g.opacity);
    settings[gp + "_size"] = length_text(g.size);
}

/// The title stored under keys "<prefix>" (text), "<prefix>_shown", "_size", "_position",
/// "_color", "_outline" and those of its decoration.
Title read_title(const Settings& settings, const std::string& prefix, TitlePosition fallback) {
    Title title;
    title.text = value_of(settings, prefix);
    title.shown = value_of(settings, prefix + "_shown") != "0";
    title.size = parse_size(value_of(settings, prefix + "_size"));
    title.position = parse_title_position(value_of(settings, prefix + "_position")).value_or(fallback);
    title.color = parse_color(settings, prefix + "_color", kWhite);
    title.outlined = value_of(settings, prefix + "_outlined") == "1";
    title.outline = parse_color(settings, prefix + "_outline", kBlack);
    title.decoration = read_decoration(settings, prefix);
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
    put_decoration(settings, prefix, title.decoration);
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
    return TitleLayout::of(title);
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
