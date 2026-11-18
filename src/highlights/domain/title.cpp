// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/domain/title.hpp"

#include <algorithm>
#include <cctype>
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

Title TitleLayout::applied_to(Title title) const {
    title.size = size;
    title.position = position;
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
