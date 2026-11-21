// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/domain/title.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ttrally::highlights {

/// A font the titles can be written in.
struct FontChoice {
    std::filesystem::path file;
    std::string name; ///< e.g. "Inter Semi Bold"
};

/// Folder of the library's own SVG shapes for title backdrops: <library>/ttrally-data/title-shapes.
inline constexpr std::string_view kTitleShapesFolder = "title-shapes";

/// A rendered title (its letters with border, backdrop, shadow and glow) as RGBA with straight
/// (not premultiplied) alpha, row by row, cut to what is drawn.
struct TitleImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

/// Port: the fonts of this machine, and titles written in them (for previews).
class TitleFonts {
  public:
    virtual ~TitleFonts() = default;
    /// The installed fonts that can write Latin text, sorted by name (may take a few seconds).
    [[nodiscard]] virtual std::vector<FontChoice> installed() = 0;
    /// The font used when none is chosen; empty if none was found.
    [[nodiscard]] virtual FontChoice fallback() = 0;
    /// The title in the font (empty: the fallback), lines `line_height` pixels apart, aligned
    /// for its position, with its decoration; an SVG backdrop is read from `shapes`. Throws
    /// std::runtime_error if the font or the SVG file cannot be read.
    [[nodiscard]] virtual TitleImage render(const std::filesystem::path& font, const Title& title,
                                            int line_height,
                                            const std::filesystem::path& shapes) = 0;
    /// File names of the SVG shapes in the folder, sorted; empty if it does not exist.
    [[nodiscard]] virtual std::vector<std::string>
    shapes(const std::filesystem::path& folder) = 0;
};

} // namespace ttrally::highlights
