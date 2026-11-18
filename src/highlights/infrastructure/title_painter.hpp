// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/domain/title.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace ttrally::highlights {

/// A rendered title: coverage (0-255) of the letters and of their dark outline, row by row.
struct TitleMask {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> outline;
};

/// Horizontal alignment of the lines of a title.
enum class TextAlign { Left, Center, Right };

/// A TrueType font that renders titles (UTF-8, one line per "\n").
class TitleFont {
  public:
    /// Throws std::runtime_error if the file cannot be read or is not a TrueType font.
    explicit TitleFont(const std::filesystem::path& file);
    ~TitleFont();
    TitleFont(const TitleFont&) = delete;
    TitleFont& operator=(const TitleFont&) = delete;

    /// The text with lines `line_height` pixels apart, with an outline around the letters.
    [[nodiscard]] TitleMask render(std::string_view text, int line_height, TextAlign align) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// A bold sans-serif system font (DejaVu, Liberation, Noto; Segoe UI or Arial on Windows); empty
/// if none of the usual files exists.
[[nodiscard]] std::filesystem::path default_title_font();

/// The alignment that suits a position (left, centred or right).
[[nodiscard]] TextAlign align_for(TitlePosition position) noexcept;

/// Planes of a YUV 4:2:0 picture to paint on.
struct YuvPlanes {
    std::uint8_t* y = nullptr;
    std::uint8_t* u = nullptr;
    std::uint8_t* v = nullptr;
    int y_stride = 0;
    int uv_stride = 0;
    int width = 0;
    int height = 0;
    bool full_range = false; ///< 0-255 instead of 16-235
};

/// Paints a title in white with a black outline at its position, a small margin away from the
/// picture's edges.
void paint_title(const YuvPlanes& picture, const TitleMask& mask, TitlePosition position);

} // namespace ttrally::highlights
