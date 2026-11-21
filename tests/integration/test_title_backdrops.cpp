// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/svg_shape.hpp"
#include "highlights/infrastructure/title_composer.hpp"
#include "highlights/infrastructure/title_painter.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <stdexcept>

using namespace ttrally::highlights;

namespace {

constexpr int kLineHeight = 40;
constexpr Rgb kBlue{0, 0, 255};

/// Letters as solid blocks, one line per width, left-aligned.
TitleMask blocks(const std::vector<int>& widths) {
    constexpr int kPad = 3;
    TitleMask mask;
    mask.width = *std::ranges::max_element(widths) + 2 * kPad;
    mask.height = kLineHeight * static_cast<int>(widths.size()) + 2 * kPad;
    mask.text.assign(static_cast<std::size_t>(mask.width * mask.height), 0);
    for (std::size_t l = 0; l < widths.size(); ++l) {
        const int top = kPad + static_cast<int>(l) * kLineHeight;
        mask.lines.push_back({kPad, top, kPad + widths[l], top + kLineHeight});
        for (int y = top + kLineHeight / 5; y < top + kLineHeight * 4 / 5; ++y) {
            for (int x = kPad; x < kPad + widths[l]; ++x) {
                mask.text[static_cast<std::size_t>(y * mask.width + x)] = 255;
            }
        }
    }
    mask.outline = mask.text;
    return mask;
}

struct Pixel {
    int r;
    int g;
    int b;
    int a;
};

Pixel pixel(const TitleImage& image, int x, int y) {
    const auto i = static_cast<std::size_t>(y * image.width + x) * 4;
    return {image.rgba[i], image.rgba[i + 1], image.rgba[i + 2], image.rgba[i + 3]};
}

template <typename Predicate> int count(const TitleImage& image, Predicate predicate) {
    int found = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            found += predicate(pixel(image, x, y)) ? 1 : 0;
        }
    }
    return found;
}

bool is_backdrop(Pixel p) { return p.r < 10 && p.g < 10 && p.b > 245 && p.a > 0; }
bool is_letter(Pixel p) { return p.r == 255 && p.g == 255 && p.b == 255 && p.a == 255; }

Title with(Backdrop backdrop) {
    Title title{.text = "x", .shown = true};
    title.decoration.backdrop = std::move(backdrop);
    return title;
}

/// Mean row of the letters' pixels, as share of the image height.
double letters_height_share(const TitleImage& image) {
    double sum = 0;
    int n = 0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (is_letter(pixel(image, x, y))) {
                sum += y;
                ++n;
            }
        }
    }
    return n == 0 ? 0 : sum / n / image.height;
}

std::filesystem::path temporary_folder(const std::string& name) {
    const auto folder = std::filesystem::temp_directory_path() /
                        std::format("ttrally-{}-{}", name, std::random_device{}());
    std::filesystem::create_directories(folder);
    return folder;
}

/// 300 x 100 with a slanted left end: empty above the line from (0, 100) to (100, 0).
constexpr const char* kSlantedSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="100" viewBox="0 0 300 100">)"
    R"(<polygon points="0,100 100,0 300,0 300,100" fill="#000"/></svg>)";

} // namespace

TEST_CASE("every built-in backdrop lies around the text in its colour and opacity") {
    const TitleMask letters = blocks({200});
    for (const BackdropShape shape : kBuiltInBackdrops) {
        if (shape == BackdropShape::None) {
            continue;
        }
        INFO(to_string(shape));
        const Title title = with({.shape = shape, .color = kBlue, .opacity = 50});
        const TitleImage image = compose_title(letters, title, kLineHeight, nullptr);
        CHECK(image.width > letters.width);
        CHECK(image.height > letters.height);
        CHECK(count(image, is_letter) > 0);
        // At least half opaque where the shape lies (two strips over each other: more)
        const int half = count(image, [](Pixel p) {
            return p.r < 10 && p.g < 10 && p.b > 150 && p.a >= 126 && p.a < 255;
        });
        CHECK(half > count(image, is_letter) / 2);
        // The same title is drawn the same way
        CHECK(compose_title(letters, title, kLineHeight, nullptr).rgba == image.rgba);
        // Another seed changes exactly the shapes drawn by chance
        Title rerolled = title;
        rerolled.decoration.backdrop.seed = next_seed(title.decoration.backdrop.seed);
        const bool same = compose_title(letters, rerolled, kLineHeight, nullptr).rgba == image.rgba;
        CHECK(same == !uses_seed(shape));
    }
}

TEST_CASE("without a backdrop only the letters are drawn, at the letters' size") {
    const TitleMask letters = blocks({120});
    const TitleImage image = compose_title(letters, Title{.text = "x", .shown = true},
                                           kLineHeight, nullptr);
    CHECK(image.width == letters.width);
    CHECK(image.height == letters.height);
    CHECK(count(image, [](Pixel p) { return p.a > 0 && !is_letter(p); }) == 0);
}

TEST_CASE("a backdrop per line follows each line's width") {
    const TitleMask letters = blocks({400, 100});
    Backdrop backdrop{.shape = BackdropShape::RoundedBox, .color = kBlue, .opacity = 100};
    const auto drawn = [&] {
        return count(compose_title(letters, with(backdrop), kLineHeight, nullptr), is_backdrop);
    };
    const int around_all = drawn();
    backdrop.per_line = true;
    const int per_line = drawn();
    CHECK(per_line < around_all * 4 / 5);
}

TEST_CASE("an invisible backdrop can still have its line along the edge") {
    const TitleMask letters = blocks({200});
    const TitleImage image = compose_title(
        letters,
        with({.shape = BackdropShape::Pill, .color = kBlue, .opacity = 0, .bordered = true,
              .border = {255, 0, 0}, .border_width = 0.1}),
        kLineHeight, nullptr);
    CHECK(count(image, is_backdrop) == 0);
    CHECK(count(image, [](Pixel p) { return p.r == 255 && p.g == 0 && p.a == 255; }) > 100);
}

TEST_CASE("shadow and glow are drawn around the text") {
    const TitleMask letters = blocks({200});
    Title title{.text = "x", .shown = true};
    title.decoration.shadow = {
        .shown = true, .color = {255, 0, 0}, .opacity = 100, .offset = 0.2, .blur = 0.1};
    const TitleImage shadowed = compose_title(letters, title, kLineHeight, nullptr);
    CHECK(shadowed.width > letters.width);
    CHECK(count(shadowed, [](Pixel p) { return p.r > 200 && p.g < 50 && p.a > 100; }) > 100);

    title.decoration.shadow.shown = false;
    title.decoration.glow = {.shown = true, .color = {0, 255, 0}, .opacity = 100, .size = 0.3};
    const TitleImage glowing = compose_title(letters, title, kLineHeight, nullptr);
    CHECK(count(glowing, [](Pixel p) {
              return p.g == 255 && p.r < 50 && p.a > 0 && p.a < 255;
          }) > 100);
}

TEST_CASE("a speech bubble points to the nearer edge of the picture") {
    const TitleMask letters = blocks({300});
    Title title = with({.shape = BackdropShape::SpeechBubble, .color = kBlue, .opacity = 100});
    title.position = TitlePosition::BottomCenter;
    CHECK(letters_height_share(compose_title(letters, title, kLineHeight, nullptr)) < 0.45);
    title.position = TitlePosition::TopCenter;
    CHECK(letters_height_share(compose_title(letters, title, kLineHeight, nullptr)) > 0.55);
}

TEST_CASE("SVG shapes keep their ends and stretch their middle") {
    const auto folder = temporary_folder("shapes");
    std::ofstream(folder / "banner.svg") << kSlantedSvg;
    std::ofstream(folder / "notes.txt") << "not a shape";
    std::ofstream(folder / "broken.svg") << "<svg";
    CHECK(svg_files_in(folder) == std::vector<std::string>{"banner.svg", "broken.svg"});
    CHECK(svg_files_in(folder / "missing").empty());
    CHECK_THROWS_AS(SvgShape(folder / "broken.svg"), std::runtime_error);

    const SvgShape shape(folder / "banner.svg");
    const auto at = [](const std::vector<float>& plane, int width, int x, int y) {
        return plane[static_cast<std::size_t>(y * width + x)];
    };
    const std::vector<float> wide = shape.coverage(1000, 100);
    CHECK(at(wide, 1000, 50, 20) < 0.05F); // the slanted end, not stretched
    CHECK(at(wide, 1000, 50, 80) > 0.95F);
    CHECK(at(wide, 1000, 500, 50) > 0.95F);
    CHECK(at(wide, 1000, 995, 50) > 0.95F);
    const std::vector<float> narrow = shape.coverage(100, 100); // squeezed as a whole
    CHECK(at(narrow, 100, 10, 50) < 0.05F);
    CHECK(at(narrow, 100, 10, 98) > 0.5F);

    SvgShapeCache cache;
    CHECK(&cache.load(folder / "banner.svg") == &cache.load(folder / "banner.svg"));
    CHECK_THROWS_AS(cache.load(folder / "missing.svg"), std::runtime_error);

    const TitleMask letters = blocks({300});
    const Title title = with({.shape = BackdropShape::Svg, .svg = "banner.svg", .color = kBlue,
                              .opacity = 100});
    CHECK(count(compose_title(letters, title, kLineHeight, &shape), is_backdrop) > 1000);
    // Without the drawing (e.g. the file is gone) only the letters are drawn
    CHECK(count(compose_title(letters, title, kLineHeight, nullptr), is_backdrop) == 0);
    std::filesystem::remove_all(folder);
}

TEST_CASE("backdrop gallery (written to TTRALLY_SCREENSHOT_DIR)") {
    const char* folder = std::getenv("TTRALLY_SCREENSHOT_DIR");
    const auto font_file = default_title_font();
    if (folder == nullptr || font_file.empty()) {
        SKIP("set TTRALLY_SCREENSHOT_DIR and install a font to see the backdrops");
    }
    const TitleFont font(font_file);
    constexpr int kWidth = 1280;
    constexpr int kRowHeight = 190;
    constexpr int kLine = 44;
    std::vector<std::pair<std::string, Title>> samples;
    for (const BackdropShape shape : kBuiltInBackdrops) {
        Title title{.text = std::format("Weiss - Ohrstedt ({})", to_string(shape)), .shown = true,
                    .position = TitlePosition::TopLeft};
        title.decoration.backdrop = {.shape = shape, .color = {20, 60, 160}, .opacity = 85};
        samples.emplace_back(std::string(to_string(shape)), title);
    }
    Title effects{.text = "Shadow, glow, soft edge\nand a line", .shown = true,
                  .position = TitlePosition::TopLeft, .color = {255, 230, 120}};
    effects.decoration = {.backdrop = {.shape = BackdropShape::Cloud, .color = {250, 250, 250},
                                       .opacity = 60, .softness = 0.2, .bordered = true,
                                       .border = {255, 120, 0}},
                          .shadow = {.shown = true},
                          .glow = {.shown = true, .color = {255, 80, 0}}};
    samples.emplace_back("effects", effects);
    Title per_line{.text = "Kreisliga\nWeiss - Ohrstedt 3:1", .shown = true,
                   .position = TitlePosition::TopLeft};
    per_line.decoration.backdrop = {.shape = BackdropShape::Brush, .color = {180, 20, 30},
                                    .opacity = 90, .per_line = true};
    samples.emplace_back("per-line", per_line);

    const int height = kRowHeight * static_cast<int>(samples.size());
    std::vector<std::uint8_t> y(static_cast<std::size_t>(kWidth * height));
    std::vector<std::uint8_t> u(static_cast<std::size_t>(kWidth * height / 4), 128);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(kWidth * height / 4), 128);
    for (int row = 0; row < height; ++row) { // a grey gradient with stripes, like a hall
        for (int x = 0; x < kWidth; ++x) {
            y[static_cast<std::size_t>(row * kWidth + x)] =
                static_cast<std::uint8_t>(60 + (x * 120 / kWidth) + ((x / 40) % 2) * 20);
        }
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Title& title = samples[i].second;
        const TitleMask letters = font.render(title.text, kLine, TextAlign::Left);
        const PaintableTitle painted =
            paintable(compose_title(letters, title, kLine, nullptr), true);
        const int top = static_cast<int>(i) * kRowHeight;
        paint_title({.y = y.data() + static_cast<std::ptrdiff_t>(top) * kWidth,
                     .u = u.data() + static_cast<std::ptrdiff_t>(top / 2) * (kWidth / 2),
                     .v = v.data() + static_cast<std::ptrdiff_t>(top / 2) * (kWidth / 2),
                     .y_stride = kWidth, .uv_stride = kWidth / 2, .width = kWidth,
                     .height = kRowHeight,
                     .full_range = true},
                    painted, TitlePosition::TopLeft);
    }
    std::ofstream out(std::filesystem::path(folder) / "backdrops.yuv", std::ios::binary);
    out.write(reinterpret_cast<const char*>(y.data()), static_cast<std::streamsize>(y.size()));
    out.write(reinterpret_cast<const char*>(u.data()), static_cast<std::streamsize>(u.size()));
    out.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
    SUCCEED(std::format("{}x{} yuv420p (full range) written", kWidth, height));
}
