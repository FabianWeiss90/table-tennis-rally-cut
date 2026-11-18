// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/title_painter.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace ttrally::highlights {

namespace {

constexpr double kOutlineShare = 0.07; ///< outline width per line height
constexpr double kMarginShare = 0.03;  ///< distance to the picture's edges per picture height
constexpr int kOpaque = 255;
constexpr int kChromaNeutral = 128;
constexpr int kFullWhite = 255;
constexpr int kFullBlack = 0;
constexpr int kLimitedWhite = 235;
constexpr int kLimitedBlack = 16;

/// Code points of UTF-8 text; invalid bytes become U+FFFD.
std::vector<char32_t> decode_utf8(std::string_view text) {
    std::vector<char32_t> code_points;
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        int length = 1;
        char32_t value = byte;
        if (byte >= 0xF0) {
            length = 4;
            value = byte & 0x07U;
        } else if (byte >= 0xE0) {
            length = 3;
            value = byte & 0x0FU;
        } else if (byte >= 0xC0) {
            length = 2;
            value = byte & 0x1FU;
        } else if (byte >= 0x80) {
            code_points.push_back(U'\uFFFD');
            ++i;
            continue;
        }
        if (i + static_cast<std::size_t>(length) > text.size()) {
            code_points.push_back(U'\uFFFD');
            break;
        }
        for (int k = 1; k < length; ++k) {
            value = (value << 6U) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
        }
        code_points.push_back(value);
        i += static_cast<std::size_t>(length);
    }
    return code_points;
}

std::vector<std::vector<char32_t>> lines_of(std::string_view text) {
    std::vector<std::vector<char32_t>> lines(1);
    for (const char32_t c : decode_utf8(text)) {
        if (c == U'\n') {
            lines.emplace_back();
        } else if (c != U'\r') {
            lines.back().push_back(c);
        }
    }
    return lines;
}

/// Every pixel takes the largest coverage within `radius` (a disc).
std::vector<std::uint8_t> dilate(const std::vector<std::uint8_t>& mask, int width, int height,
                                 int radius) {
    std::vector<std::uint8_t> result(mask.size(), 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t best = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sy = y + dy;
                if (sy < 0 || sy >= height) {
                    continue;
                }
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sx = x + dx;
                    if (sx < 0 || sx >= width || dx * dx + dy * dy > radius * radius) {
                        continue;
                    }
                    best = std::max(best, mask[static_cast<std::size_t>(sy * width + sx)]);
                }
            }
            result[static_cast<std::size_t>(y * width + x)] = best;
        }
    }
    return result;
}

std::uint8_t mix(int base, int target, int alpha) {
    return static_cast<std::uint8_t>((base * (kOpaque - alpha) + target * alpha + kOpaque / 2) /
                                     kOpaque);
}

} // namespace

struct TitleFont::Impl {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

TitleFont::TitleFont(const std::filesystem::path& file) : impl_(std::make_unique<Impl>()) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read the font " + file.string());
    }
    impl_->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const int offset = stbtt_GetFontOffsetForIndex(impl_->data.data(), 0);
    if (offset < 0 || stbtt_InitFont(&impl_->info, impl_->data.data(), offset) == 0) {
        throw std::runtime_error(file.string() + " is not a TrueType font");
    }
}

TitleFont::~TitleFont() = default;

TitleMask TitleFont::render(std::string_view text, int line_height, TextAlign align) const {
    const stbtt_fontinfo& font = impl_->info;
    const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(line_height));
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    const int baseline = static_cast<int>(std::lround(ascent * scale));
    const int advance = static_cast<int>(std::lround((ascent - descent + gap) * scale));
    const int outline = std::max(1, static_cast<int>(std::lround(line_height * kOutlineShare)));
    const int pad = outline + 1;

    const auto lines = lines_of(text);
    std::vector<int> widths;
    for (const auto& line : lines) {
        float x = 0;
        for (std::size_t i = 0; i < line.size(); ++i) {
            int glyph_advance = 0;
            int bearing = 0;
            stbtt_GetCodepointHMetrics(&font, static_cast<int>(line[i]), &glyph_advance, &bearing);
            x += static_cast<float>(glyph_advance) * scale;
            if (i + 1 < line.size()) {
                x += static_cast<float>(stbtt_GetCodepointKernAdvance(
                         &font, static_cast<int>(line[i]), static_cast<int>(line[i + 1]))) *
                     scale;
            }
        }
        widths.push_back(static_cast<int>(std::ceil(x)));
    }
    TitleMask mask;
    mask.width = *std::ranges::max_element(widths) + 2 * pad;
    mask.height = advance * static_cast<int>(lines.size()) + 2 * pad;
    mask.text.assign(static_cast<std::size_t>(mask.width * mask.height), 0);

    std::vector<unsigned char> glyph;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const auto& line = lines[l];
        const int free = mask.width - 2 * pad - widths[l];
        float x = static_cast<float>(pad + (align == TextAlign::Left     ? 0
                                            : align == TextAlign::Center ? free / 2
                                                                         : free));
        const int y = pad + static_cast<int>(l) * advance + baseline;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const int c = static_cast<int>(line[i]);
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            const float shift = x - std::floor(x);
            stbtt_GetCodepointBitmapBoxSubpixel(&font, c, scale, scale, shift, 0, &x0, &y0, &x1,
                                                &y1);
            const int w = x1 - x0;
            const int h = y1 - y0;
            if (w > 0 && h > 0) {
                glyph.assign(static_cast<std::size_t>(w * h), 0);
                stbtt_MakeCodepointBitmapSubpixel(&font, glyph.data(), w, h, w, scale, scale,
                                                  shift, 0, c);
                const int left = static_cast<int>(std::floor(x)) + x0;
                for (int gy = 0; gy < h; ++gy) {
                    const int my = y + y0 + gy;
                    if (my < 0 || my >= mask.height) {
                        continue;
                    }
                    for (int gx = 0; gx < w; ++gx) {
                        const int mx = left + gx;
                        if (mx < 0 || mx >= mask.width) {
                            continue;
                        }
                        auto& target = mask.text[static_cast<std::size_t>(my * mask.width + mx)];
                        target = std::max(target, glyph[static_cast<std::size_t>(gy * w + gx)]);
                    }
                }
            }
            int glyph_advance = 0;
            int bearing = 0;
            stbtt_GetCodepointHMetrics(&font, c, &glyph_advance, &bearing);
            x += static_cast<float>(glyph_advance) * scale;
            if (i + 1 < line.size()) {
                x += static_cast<float>(
                         stbtt_GetCodepointKernAdvance(&font, c, static_cast<int>(line[i + 1]))) *
                     scale;
            }
        }
    }
    mask.outline = dilate(mask.text, mask.width, mask.height, outline);
    return mask;
}

std::filesystem::path default_title_font() {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    const char* windows = std::getenv("WINDIR");
    const std::filesystem::path fonts =
        std::filesystem::path(windows != nullptr ? windows : "C:\\Windows") / "Fonts";
    candidates = {fonts / "segoeuib.ttf", fonts / "arialbd.ttf", fonts / "arial.ttf"};
#else
    candidates = {
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",       // Fedora
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",         // Debian, Ubuntu
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",                     // Arch
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    };
#endif
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

TextAlign align_for(TitlePosition position) noexcept {
    switch (position) {
    case TitlePosition::TopCenter:
    case TitlePosition::BottomCenter:
        return TextAlign::Center;
    case TitlePosition::TopRight:
    case TitlePosition::BottomRight:
        return TextAlign::Right;
    default:
        return TextAlign::Left;
    }
}

void paint_title(const YuvPlanes& picture, const TitleMask& mask, TitlePosition position) {
    const int margin = static_cast<int>(std::lround(picture.height * kMarginShare));
    const TextAlign align = align_for(position);
    const int left = align == TextAlign::Left     ? margin
                     : align == TextAlign::Center ? (picture.width - mask.width) / 2
                                                  : picture.width - margin - mask.width;
    const bool top = position == TitlePosition::TopLeft || position == TitlePosition::TopCenter ||
                     position == TitlePosition::TopRight;
    const int top_row = top ? margin : picture.height - margin - mask.height;
    const int white = picture.full_range ? kFullWhite : kLimitedWhite;
    const int black = picture.full_range ? kFullBlack : kLimitedBlack;

    auto coverage = [&](const std::vector<std::uint8_t>& plane, int x, int y) -> int {
        const int mx = x - left;
        const int my = y - top_row;
        if (mx < 0 || my < 0 || mx >= mask.width || my >= mask.height) {
            return 0;
        }
        return plane[static_cast<std::size_t>(my * mask.width + mx)];
    };
    const int x_begin = std::max(0, left);
    const int x_end = std::min(picture.width, left + mask.width);
    const int y_begin = std::max(0, top_row);
    const int y_end = std::min(picture.height, top_row + mask.height);
    for (int y = y_begin; y < y_end; ++y) {
        std::uint8_t* row = picture.y + static_cast<std::ptrdiff_t>(y) * picture.y_stride;
        for (int x = x_begin; x < x_end; ++x) {
            const int dark = coverage(mask.outline, x, y);
            const int light = coverage(mask.text, x, y);
            row[x] = mix(mix(row[x], black, dark), white, light);
        }
    }
    // Chroma: towards grey wherever the title (letters or outline) covers the 2x2 block
    for (int cy = y_begin / 2; cy < (y_end + 1) / 2; ++cy) {
        std::uint8_t* u_row = picture.u + static_cast<std::ptrdiff_t>(cy) * picture.uv_stride;
        std::uint8_t* v_row = picture.v + static_cast<std::ptrdiff_t>(cy) * picture.uv_stride;
        for (int cx = x_begin / 2; cx < (x_end + 1) / 2; ++cx) {
            int sum = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    sum += coverage(mask.outline, 2 * cx + dx, 2 * cy + dy);
                }
            }
            const int alpha = sum / 4;
            u_row[cx] = mix(u_row[cx], kChromaNeutral, alpha);
            v_row[cx] = mix(v_row[cx], kChromaNeutral, alpha);
        }
    }
}

} // namespace ttrally::highlights
