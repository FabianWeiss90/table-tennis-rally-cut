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

/// The shape behind a title's text.
enum class BackdropShape {
    None,
    RoundedBox,   ///< a box with round corners
    Pill,         ///< a box with half-round ends
    Cloud,        ///< round bumps around the text
    TornEnds,     ///< a band, its ends torn off
    PaperStrip,   ///< a strip torn from a roll: slightly uneven edges, torn ends
    Tape,         ///< a slightly tilted piece of tape with finely jagged, slanted ends
    DoubleStrip,  ///< two paper strips, the one behind turned a little and darker
    Brush,        ///< a broad brush stroke with frayed edges and bristly ends
    SpeechBubble, ///< a rounded bubble with a tip towards the picture's edge
    Ribbon,       ///< a band with V-notched ends and folds behind
    SportBar,     ///< a parallelogram, as in sports TV graphics
    Svg,          ///< an SVG file from the shapes folder, in the backdrop's colour
};

/// The built-in shapes, in the order they are offered (without Svg).
inline constexpr std::array kBuiltInBackdrops{
    BackdropShape::None,        BackdropShape::RoundedBox,   BackdropShape::Pill,
    BackdropShape::Cloud,       BackdropShape::TornEnds,     BackdropShape::PaperStrip,
    BackdropShape::Tape,        BackdropShape::DoubleStrip,  BackdropShape::Brush,
    BackdropShape::SpeechBubble, BackdropShape::Ribbon,      BackdropShape::SportBar,
};

/// Name used in files, e.g. "torn-ends".
[[nodiscard]] std::string_view to_string(BackdropShape shape) noexcept;
[[nodiscard]] std::optional<BackdropShape> parse_backdrop_shape(std::string_view text) noexcept;
/// Whether the shape is drawn with chance (bumps, tears, bristles), from the backdrop's seed.
[[nodiscard]] bool uses_seed(BackdropShape shape) noexcept;
/// Another seed, for "Reroll": a different shape of the same kind.
[[nodiscard]] std::uint32_t next_seed(std::uint32_t seed) noexcept;

/// Opacities are percentages.
inline constexpr int kMaxOpacity = 100;

/// A shape behind the text. Lengths are shares of the line height.
struct Backdrop {
    static constexpr double kMaxLength = 1.5; ///< of padding, corner, softness
    static constexpr double kMaxBorder = 0.3;

    BackdropShape shape = BackdropShape::None;
    std::string svg{}; ///< file name in the shapes folder, for BackdropShape::Svg
    Rgb color = kBlack;
    int opacity = 70;
    bool per_line = false; ///< one shape per line instead of one around all lines
    std::uint32_t seed = 1;
    double padding = 0.3;   ///< free space between the text and the shape's inner edge
    double corner = 0.35;   ///< radius of the rounded box's corners
    double softness = 0.0;  ///< width over which the edge fades out
    bool bordered = false;  ///< with a line along the shape's edge (opaque)
    Rgb border = kWhite;
    double border_width = 0.06;

    [[nodiscard]] bool visible() const noexcept;
    friend bool operator==(const Backdrop&, const Backdrop&) = default;
};

/// A blurred copy of the backdrop (or, without one, of the text) shifted down to the right.
struct TitleShadow {
    static constexpr double kMaxLength = 0.5; ///< of offset and blur

    bool shown = false;
    Rgb color = kBlack;
    int opacity = 60;
    double offset = 0.08; ///< per line height
    double blur = 0.12;   ///< per line height
    friend bool operator==(const TitleShadow&, const TitleShadow&) = default;
};

/// A soft light around the letters.
struct TitleGlow {
    static constexpr double kMaxSize = 0.5;

    bool shown = false;
    Rgb color = kWhite;
    int opacity = 80;
    double size = 0.15; ///< per line height
    friend bool operator==(const TitleGlow&, const TitleGlow&) = default;
};

/// What is drawn around a title's letters.
struct TitleDecoration {
    Backdrop backdrop;
    TitleShadow shadow;
    TitleGlow glow;
    friend bool operator==(const TitleDecoration&, const TitleDecoration&) = default;
};

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
    TitleDecoration decoration{};

    /// Whether anything is drawn.
    [[nodiscard]] bool visible() const noexcept;
    friend bool operator==(const Title&, const Title&) = default;
};

/// Everything that changes how a title looks, as text (to tell rendered titles apart).
[[nodiscard]] std::string look_key(const Title& title);

/// Size, position and colours of titles, e.g. the same for every game in a match day video.
struct TitleLayout {
    double size = Title::kDefaultSize; ///< height of a line as share of the picture height
    TitlePosition position = TitlePosition::BottomCenter;
    Rgb color = kWhite;
    bool outlined = false;
    Rgb outline = kBlack;
    TitleDecoration decoration{};

    /// The layout of a title.
    [[nodiscard]] static TitleLayout of(const Title& title);
    /// The title with this size, position, colours and decoration.
    [[nodiscard]] Title applied_to(Title title) const;
    friend bool operator==(const TitleLayout&, const TitleLayout&) = default;
};

/// The players named in a video's file name: "Weiss-Ohrstedt" -> "Weiss - Ohrstedt",
/// "weiss-belger_greta" -> "Weiss - Belger Greta". Names are split at the first "-", or at a
/// word "vs" if there is one; "_" becomes a space, a number at the end (e.g. the second game,
/// "Kroekel2") is dropped and every word starts with a capital letter. Empty if the name does
/// not hold two parts (e.g. a camera's file name). Doubles videos start with "Doppel" or
/// "Doubles" (any case): "Doppel-Weiss_Becker-Mueller_Schmidt" -> "Weiss/Becker -
/// Mueller/Schmidt", the players of a pair split at "_".
[[nodiscard]] std::string title_from_file_name(std::string_view stem);

/// The match of a match day folder: "2026-11-14-WolmirstedtII-Bismark" -> "WolmirstedtII -
/// Bismark" (the date in front is dropped, the rest is read as title_from_file_name does). Empty
/// if the name does not hold two teams.
[[nodiscard]] std::string match_from_folder_name(std::string_view folder);

/// A match day folder's name without the date in front: "2026-11-21-Haldensleben_II-Bismark"
/// -> "Haldensleben_II-Bismark"; the name itself if it has no date (or only one).
[[nodiscard]] std::string without_date(std::string_view folder);

/// The title of a match day's match when none was saved: from the folder name, at the top centre.
[[nodiscard]] Title default_match_title(std::string_view folder);

/// The text with the two sides of " - " (or of " vs " in older titles) in its first line swapped;
/// unchanged without either.
[[nodiscard]] std::string swap_players(std::string_view text);

} // namespace ttrally::highlights
