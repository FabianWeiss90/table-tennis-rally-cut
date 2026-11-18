// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ttrally::highlights {

/// Where a title sits in the picture.
enum class TitlePosition {
    TopLeft,
    TopCenter,
    TopRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

inline constexpr std::array kTitlePositions{
    TitlePosition::TopLeft,    TitlePosition::TopCenter,    TitlePosition::TopRight,
    TitlePosition::BottomLeft, TitlePosition::BottomCenter, TitlePosition::BottomRight,
};

/// Name used in files and on the command line, e.g. "top-left".
[[nodiscard]] std::string_view to_string(TitlePosition position) noexcept;
[[nodiscard]] std::optional<TitlePosition> parse_title_position(std::string_view text) noexcept;

/// A colour of a title.
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    friend bool operator==(const Rgb&, const Rgb&) = default;
};

inline constexpr Rgb kWhite{255, 255, 255};
inline constexpr Rgb kBlack{0, 0, 0};

/// "#rrggbb", as in files.
[[nodiscard]] std::string to_hex(Rgb color);
/// Parses "#rrggbb" (or without "#"); nullopt if it is not one.
[[nodiscard]] std::optional<Rgb> parse_hex(std::string_view text) noexcept;

/// Text shown over clips, e.g. the players "Weiss - Ohrstedt" or the match "Team A - Team B";
/// may have several lines.
struct Title {
    static constexpr double kDefaultSize = 0.05;
    static constexpr double kMinSize = 0.02;
    static constexpr double kMaxSize = 0.2;

    std::string text;
    bool shown = true;
    double size = kDefaultSize; ///< height of a line as share of the picture height
    TitlePosition position = TitlePosition::BottomCenter;
    Rgb color = kWhite;    ///< of the letters
    bool outlined = false; ///< with a border around the letters
    Rgb outline = kBlack;  ///< colour of the border

    /// Whether anything is drawn.
    [[nodiscard]] bool visible() const noexcept;
    friend bool operator==(const Title&, const Title&) = default;
};

/// Size, position and colours of titles, e.g. the same for every game in a match day video.
struct TitleLayout {
    double size = Title::kDefaultSize; ///< height of a line as share of the picture height
    TitlePosition position = TitlePosition::BottomCenter;
    Rgb color = kWhite;
    bool outlined = false;
    Rgb outline = kBlack;

    /// The title with this size, position and colours.
    [[nodiscard]] Title applied_to(Title title) const;
    friend bool operator==(const TitleLayout&, const TitleLayout&) = default;
};

/// The players named in a video's file name: "Weiss-Ohrstedt" -> "Weiss - Ohrstedt",
/// "weiss-belger_greta" -> "Weiss - Belger Greta". Names are split at the first "-", or at a
/// word "vs" if there is one; "_" becomes a space, a number at the end (e.g. the second game,
/// "Kroekel2") is dropped and every word starts with a capital letter. Empty if the name does
/// not hold two parts (e.g. a camera's file name).
[[nodiscard]] std::string title_from_file_name(std::string_view stem);

/// The match of a match day folder: "2026-11-14-WolmirstedtII-Bismark" -> "WolmirstedtII -
/// Bismark" (the date in front is dropped, the rest is read as title_from_file_name does). Empty
/// if the name does not hold two teams.
[[nodiscard]] std::string match_from_folder_name(std::string_view folder);

/// The title of a match day's match when none was saved: from the folder name, at the top centre.
[[nodiscard]] Title default_match_title(std::string_view folder);

/// The text with the two sides of " - " (or of " vs " in older titles) in its first line swapped;
/// unchanged without either.
[[nodiscard]] std::string swap_players(std::string_view text);

} // namespace ttrally::highlights
