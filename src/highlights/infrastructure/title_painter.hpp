// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/title_fonts.hpp"
#include "highlights/domain/title.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace ttrally::highlights {

/// The box a line of text takes, in pixels of its mask (right and bottom exclusive).
struct TextBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

/// The letters of a title: coverage (0-255) of the letters and of the border around them, and
/// the box of each line.
struct TitleMask {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> text;
    std::vector<std::uint8_t> outline;
    std::vector<TextBox> lines;
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
    /// Family and style from the font's name table, e.g. "Inter Semi Bold"; empty if missing.
    [[nodiscard]] std::string name() const;
    /// Whether it has the letters of German text (A-Z, a-z, umlauts, ß).
    [[nodiscard]] bool writes_latin() const;

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

/// A colour in the picture's YUV values (BT.709).
struct Yuv {
    int y = 0;
    int u = 0;
    int v = 0;
};

[[nodiscard]] Yuv to_yuv(Rgb color, bool full_range) noexcept;

/// A rendered title in a picture's YUV values, ready to be painted on every picture.
struct PaintableTitle {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
    std::vector<std::uint8_t> alpha;
};

[[nodiscard]] PaintableTitle paintable(const TitleImage& image, bool full_range);

/// Paints a rendered title at its position, a small margin away from the picture's edges.
void paint_title(const YuvPlanes& picture, const PaintableTitle& title, TitlePosition position);

} // namespace ttrally::highlights
