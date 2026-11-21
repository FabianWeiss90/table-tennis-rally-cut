// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/title_composer.hpp"

#include "highlights/infrastructure/backdrop_shapes.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ttrally::highlights {

namespace {

constexpr float kFull = 255.0F;
constexpr double kBackShade = 0.7;  ///< brightness of a backdrop's parts behind
constexpr float kGlowGain = 2.5F;   ///< a blurred letter is faint: brightened for the glow
constexpr int kBlurPasses = 3;      ///< of the box blur, each with a third of the radius
constexpr int kSpare = 2;           ///< pixels around everything, for rounding

/// A coverage plane in its colour and opacity (0-1).
struct Layer {
    const std::vector<float>* coverage;
    Rgb color;
    float opacity;
};

Rgb shaded(Rgb color, double shade) {
    const auto channel = [shade](std::uint8_t c) {
        return static_cast<std::uint8_t>(std::lround(c * shade));
    };
    return {channel(color.r), channel(color.g), channel(color.b)};
}

float share(int percent) {
    return static_cast<float>(std::clamp(percent, 0, kMaxOpacity)) / kMaxOpacity;
}

/// The boxes of the backdrop in the letters' pixels: each line's (or one around all lines),
/// grown by the padding. Empty lines get none.
std::vector<ShapeBox> boxes_of(const TitleMask& letters, const Backdrop& backdrop,
                               double line_height) {
    std::vector<ShapeBox> boxes;
    const double pad = backdrop.padding * line_height;
    for (const TextBox& line : letters.lines) {
        if (line.right <= line.left) {
            continue;
        }
        const ShapeBox box{line.left - pad, line.top - pad, line.right + pad, line.bottom + pad};
        if (backdrop.per_line || boxes.empty()) {
            boxes.push_back(box);
        } else {
            ShapeBox& all = boxes.front();
            all = {std::min(all.left, box.left), std::min(all.top, box.top),
                   std::max(all.right, box.right), std::max(all.bottom, box.bottom)};
        }
    }
    return boxes;
}

/// The letters' plane (0-255) as coverage on the canvas, at (offset, offset).
std::vector<float> placed(const std::vector<std::uint8_t>& mask, const TitleMask& letters,
                          int width, int height, int offset) {
    std::vector<float> plane(static_cast<std::size_t>(width) * height, 0.0F);
    for (int y = 0; y < letters.height; ++y) {
        for (int x = 0; x < letters.width; ++x) {
            plane[static_cast<std::size_t>(y + offset) * width + x + offset] =
                mask[static_cast<std::size_t>(y) * letters.width + x] / kFull;
        }
    }
    return plane;
}

/// The plane moved by (shift, shift) pixels and blurred.
std::vector<float> shadow_of(const std::vector<float>& source, int width, int height, int shift,
                             int blur_px) {
    std::vector<float> shadow(source.size(), 0.0F);
    for (int y = shift; y < height; ++y) {
        for (int x = shift; x < width; ++x) {
            shadow[static_cast<std::size_t>(y) * width + x] =
                source[static_cast<std::size_t>(y - shift) * width + x - shift];
        }
    }
    blur(shadow, width, height, blur_px / kBlurPasses);
    return shadow;
}

/// The layers over each other ("over", straight alpha), cut to what is drawn and to `keep` (the
/// letters' box, so that a title sits at the same place whatever its letters).
TitleImage flattened(const std::vector<Layer>& layers, int width, int height,
                     const TextBox& keep) {
    const auto size = static_cast<std::size_t>(width) * height;
    std::vector<float> red(size, 0.0F);
    std::vector<float> green(size, 0.0F);
    std::vector<float> blue(size, 0.0F);
    std::vector<float> alpha(size, 0.0F);
    for (const Layer& layer : layers) {
        for (std::size_t i = 0; i < size; ++i) {
            const float a = (*layer.coverage)[i] * layer.opacity;
            if (a <= 0) {
                continue;
            }
            red[i] = red[i] * (1 - a) + layer.color.r * a; // premultiplied
            green[i] = green[i] * (1 - a) + layer.color.g * a;
            blue[i] = blue[i] * (1 - a) + layer.color.b * a;
            alpha[i] = alpha[i] * (1 - a) + a;
        }
    }
    const auto drawn = [&](std::size_t i) { return alpha[i] * kFull >= 0.5F; };
    int left = keep.left;
    int right = keep.right;
    int top = keep.top;
    int bottom = keep.bottom;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (drawn(static_cast<std::size_t>(y) * width + x)) {
                left = std::min(left, x);
                right = std::max(right, x + 1);
                top = std::min(top, y);
                bottom = std::max(bottom, y + 1);
            }
        }
    }
    TitleImage image{.width = right - left, .height = bottom - top, .rgba = {}};
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    const auto channel = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, kFull)));
    };
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto i = static_cast<std::size_t>(y) * width + x;
            const auto o = (static_cast<std::size_t>(y - top) * image.width + (x - left)) * 4;
            const float a = alpha[i];
            image.rgba[o] = a > 0 ? channel(red[i] / a) : 0;
            image.rgba[o + 1] = a > 0 ? channel(green[i] / a) : 0;
            image.rgba[o + 2] = a > 0 ? channel(blue[i] / a) : 0;
            image.rgba[o + 3] = channel(a * kFull);
        }
    }
    return image;
}

} // namespace

TitleImage compose_title(const TitleMask& letters, const Title& title, int line_height,
                         const SvgShape* svg) {
    const double lh = std::max(1, line_height);
    const TitleDecoration& decoration = title.decoration;
    const Backdrop& backdrop = decoration.backdrop;
    const TitleShadow& shadow = decoration.shadow;
    const TitleGlow& glow = decoration.glow;
    std::vector<ShapeBox> boxes;
    if (backdrop.visible() && (backdrop.shape != BackdropShape::Svg || svg != nullptr)) {
        boxes = boxes_of(letters, backdrop, lh);
    }
    const int shift = shadow.shown ? static_cast<int>(std::lround(shadow.offset * lh)) : 0;
    const int shadow_blur = shadow.shown ? static_cast<int>(std::lround(shadow.blur * lh)) : 0;
    const int glow_px = glow.shown ? static_cast<int>(std::lround(glow.size * lh)) : 0;
    double room = kSpare + shift + 2 * shadow_blur + 2 * glow_px;
    if (!boxes.empty()) {
        room += backdrop.padding * lh + backdrop_reach(backdrop, boxes, lh);
    }
    const int margin = static_cast<int>(std::ceil(room));
    const int width = letters.width + 2 * margin;
    const int height = letters.height + 2 * margin;
    for (ShapeBox& box : boxes) {
        box = {box.left + margin, box.top + margin, box.right + margin, box.bottom + margin};
    }

    const std::vector<float> text = placed(letters.text, letters, width, height, margin);
    const std::vector<float> outline =
        title.outlined ? placed(letters.outline, letters, width, height, margin)
                       : std::vector<float>();
    BackdropPlanes planes;
    if (!boxes.empty()) {
        planes = draw_backdrop(backdrop, boxes, width, height, lh, title.position, svg);
    }
    std::vector<Layer> layers;
    std::vector<float> shadow_plane;
    if (shadow.shown) {
        std::vector<float> source =
            boxes.empty() ? (title.outlined ? outline : text) : planes.front;
        if (!boxes.empty()) {
            for (std::size_t i = 0; i < source.size(); ++i) {
                source[i] = std::max(source[i], planes.back[i]);
            }
        } else if (title.outlined) {
            for (std::size_t i = 0; i < source.size(); ++i) {
                source[i] = std::max(source[i], text[i]);
            }
        }
        shadow_plane = shadow_of(source, width, height, shift, shadow_blur);
        layers.push_back({&shadow_plane, shadow.color, share(shadow.opacity)});
    }
    if (!boxes.empty()) {
        const float opacity = share(backdrop.opacity);
        layers.push_back({&planes.back, shaded(backdrop.color, kBackShade), opacity});
        layers.push_back({&planes.front, backdrop.color, opacity});
        if (backdrop.bordered) {
            layers.push_back({&planes.border, backdrop.border, 1.0F});
        }
    }
    std::vector<float> glow_plane;
    if (glow.shown && glow_px > 0) {
        glow_plane = title.outlined ? outline : text;
        blur(glow_plane, width, height, std::max(1, glow_px / kBlurPasses));
        for (float& value : glow_plane) {
            value = std::min(1.0F, value * kGlowGain);
        }
        layers.push_back({&glow_plane, glow.color, share(glow.opacity)});
    }
    if (title.outlined) {
        layers.push_back({&outline, title.outline, 1.0F});
    }
    layers.push_back({&text, title.color, 1.0F});
    return flattened(layers, width, height,
                     {margin, margin, margin + letters.width, margin + letters.height});
}

} // namespace ttrally::highlights
