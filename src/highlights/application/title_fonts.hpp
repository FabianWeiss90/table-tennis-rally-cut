// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/domain/title.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ttrally::highlights {

/// A font the titles can be written in.
struct FontChoice {
    std::filesystem::path file;
    std::string name; ///< e.g. "Inter Semi Bold"
};

/// A title as coverage (0-255) of its letters and of a border around them, row by row.
struct TitleImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> outline;
};

/// Port: the fonts of this machine, and titles written in them (for previews).
class TitleFonts {
  public:
    virtual ~TitleFonts() = default;
    /// The installed fonts that can write Latin text, sorted by name (may take a few seconds).
    [[nodiscard]] virtual std::vector<FontChoice> installed() = 0;
    /// The font used when none is chosen; empty if none was found.
    [[nodiscard]] virtual FontChoice fallback() = 0;
    /// The title's text in the font (empty: the fallback), lines `line_height` pixels apart,
    /// aligned for its position. Throws std::runtime_error if the font cannot be read.
    [[nodiscard]] virtual TitleImage render(const std::filesystem::path& font, const Title& title,
                                            int line_height) = 0;
};

} // namespace ttrally::highlights
