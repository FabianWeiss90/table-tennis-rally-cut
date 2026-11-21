// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/domain/title.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <vector>

namespace ttrally::highlights {

namespace {

/// Between the players in a title.
constexpr std::string_view kSeparator = " - ";
/// The separator of older titles, still recognised when swapping.
constexpr std::string_view kOldSeparator = " vs ";

constexpr std::array<std::pair<TitlePosition, std::string_view>, 6> kPositionNames{{
    {TitlePosition::TopLeft, "top-left"},
    {TitlePosition::TopCenter, "top-center"},
    {TitlePosition::TopRight, "top-right"},
    {TitlePosition::BottomLeft, "bottom-left"},
    {TitlePosition::BottomCenter, "bottom-center"},
    {TitlePosition::BottomRight, "bottom-right"},
}};

constexpr std::array<std::pair<BackdropShape, std::string_view>, 13> kBackdropNames{{
    {BackdropShape::None, "none"},
    {BackdropShape::RoundedBox, "rounded-box"},
    {BackdropShape::Pill, "pill"},
    {BackdropShape::Cloud, "cloud"},
    {BackdropShape::TornEnds, "torn-ends"},
    {BackdropShape::PaperStrip, "paper-strip"},
    {BackdropShape::Tape, "tape"},
    {BackdropShape::DoubleStrip, "double-strip"},
    {BackdropShape::Brush, "brush"},
    {BackdropShape::SpeechBubble, "speech-bubble"},
    {BackdropShape::Ribbon, "ribbon"},
    {BackdropShape::SportBar, "sport-bar"},
    {BackdropShape::Svg, "svg"},
}};

bool is_versus(std::string_view word) {
    std::string lower;
    for (const char c : word) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower == "vs" || lower == "vs.";
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : text) {
        if (c == separator) {
            parts.push_back(std::move(part));
            part.clear();
        } else {
            part += c;
        }
    }
    parts.push_back(std::move(part));
    std::erase_if(parts, [](const std::string& p) { return p.empty(); });
    return parts;
}

/// "belger_greta2" -> "Belger Greta"
std::string player_name(const std::vector<std::string>& parts) {
    std::string name;
    for (const std::string& part : parts) {
        name += (name.empty() ? "" : " ") + part;
    }
    std::ranges::replace(name, '_', ' ');
    while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    bool word_start = true;
    for (char& c : name) {
        if (word_start) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        word_start = c == ' ';
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    return name;
}

} // namespace

std::string_view to_string(TitlePosition position) noexcept {
    for (const auto& [value, name] : kPositionNames) {
        if (value == position) {
            return name;
        }
    }
    return "top-left";
}

std::optional<TitlePosition> parse_title_position(std::string_view text) noexcept {
    for (const auto& [value, name] : kPositionNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

bool Title::visible() const noexcept {
    return shown && std::ranges::any_of(text, [](char c) {
               return !std::isspace(static_cast<unsigned char>(c));
           });
}

TitleLayout TitleLayout::of(const Title& title) {
    return {.size = title.size,
            .position = title.position,
            .color = title.color,
            .outlined = title.outlined,
            .outline = title.outline,
            .decoration = title.decoration};
}

Title TitleLayout::applied_to(Title title) const {
    title.size = size;
    title.position = position;
    title.color = color;
    title.outlined = outlined;
    title.outline = outline;
    title.decoration = decoration;
    return title;
}

std::string_view to_string(BackdropShape shape) noexcept {
    for (const auto& [value, name] : kBackdropNames) {
        if (value == shape) {
            return name;
        }
    }
    return "none";
}

std::optional<BackdropShape> parse_backdrop_shape(std::string_view text) noexcept {
    for (const auto& [value, name] : kBackdropNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

bool uses_seed(BackdropShape shape) noexcept {
    switch (shape) {
    case BackdropShape::Cloud:
    case BackdropShape::TornEnds:
    case BackdropShape::PaperStrip:
    case BackdropShape::Tape:
    case BackdropShape::DoubleStrip:
    case BackdropShape::Brush:
        return true;
    default:
        return false;
    }
}

std::uint32_t next_seed(std::uint32_t seed) noexcept {
    // A step of a 32-bit linear congruential generator (Numerical Recipes), never 0
    constexpr std::uint32_t kMultiplier = 1664525U;
    constexpr std::uint32_t kIncrement = 1013904223U;
    const std::uint32_t next = seed * kMultiplier + kIncrement;
    return next == 0 ? 1 : next;
}

bool Backdrop::visible() const noexcept {
    return shape != BackdropShape::None && (opacity > 0 || bordered) &&
           (shape != BackdropShape::Svg || !svg.empty());
}

std::string look_key(const Title& title) {
    const Backdrop& b = title.decoration.backdrop;
    const TitleShadow& s = title.decoration.shadow;
    const TitleGlow& g = title.decoration.glow;
    return std::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{:.3f}|{:.3f}|{:.3f}|{}|{:.3f}|"
                       "{}|{}|{}|{:.3f}|{:.3f}|{}|{}|{}|{:.3f}",
                       title.text, to_string(title.position), to_hex(title.color), title.outlined,
                       to_hex(title.outline), to_string(b.shape), b.svg, to_hex(b.color),
                       b.opacity, b.per_line, b.seed, b.bordered, b.padding, b.corner,
                       b.softness, to_hex(b.border), b.border_width, s.shown,
                       to_hex(s.color), s.opacity, s.offset, s.blur, g.shown, to_hex(g.color),
                       g.opacity, g.size);
}

std::string to_hex(Rgb color) {
    return std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
}

std::optional<Rgb> parse_hex(std::string_view text) noexcept {
    if (text.starts_with('#')) {
        text.remove_prefix(1);
    }
    constexpr std::size_t kDigits = 6;
    if (text.size() != kDigits) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const std::string_view digits = text.substr(2 * c, 2);
        constexpr int kHex = 16;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), channels[c], kHex);
        if (error != std::errc() || end != digits.data() + digits.size()) {
            return std::nullopt;
        }
    }
    return Rgb{channels[0], channels[1], channels[2]};
}

std::string match_from_folder_name(std::string_view folder) {
    std::vector<std::string> parts = split(folder, '-');
    const auto is_number = [](const std::string& part) {
        return std::ranges::all_of(part, [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        });
    };
    const auto teams = std::ranges::find_if_not(parts, is_number);
    std::string rest;
    for (auto it = teams; it != parts.end(); ++it) {
        rest += (rest.empty() ? "" : "-") + *it;
    }
    return title_from_file_name(rest);
}

Title default_match_title(std::string_view folder) {
    Title title;
    title.text = match_from_folder_name(folder);
    title.position = TitlePosition::TopCenter;
    return title;
}

std::string title_from_file_name(std::string_view stem) {
    const std::vector<std::string> parts = split(stem, '-');
    const auto versus = std::ranges::find_if(parts.rbegin(), parts.rend(), is_versus);
    std::vector<std::string> left;
    std::vector<std::string> right;
    if (versus != parts.rend()) {
        const auto at = std::prev(versus.base());
        left.assign(parts.begin(), at);
        right.assign(std::next(at), parts.end());
    } else if (parts.size() >= 2) {
        left.assign(parts.begin(), parts.begin() + 1);
        right.assign(parts.begin() + 1, parts.end());
    }
    const std::string a = player_name(left);
    const std::string b = player_name(right);
    return a.empty() || b.empty() ? std::string() : a + std::string(kSeparator) + b;
}

std::string swap_players(std::string_view text) {
    const std::size_t line_end = std::min(text.find('\n'), text.size());
    const std::string_view first_line = text.substr(0, line_end);
    for (const std::string_view separator : {kSeparator, kOldSeparator}) {
        const std::size_t at = first_line.find(separator);
        if (at != std::string_view::npos) {
            return std::string(first_line.substr(at + separator.size())) +
                   std::string(separator) + std::string(first_line.substr(0, at)) +
                   std::string(text.substr(line_end));
        }
    }
    return std::string(text);
}

} // namespace ttrally::highlights
