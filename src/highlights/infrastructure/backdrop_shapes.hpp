// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/domain/title.hpp"

#include <span>
#include <vector>

namespace ttrally::highlights {

class SvgShape;

/// A box in pixels of a title's canvas (right and bottom exclusive).
struct ShapeBox {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
};

/// Coverage planes (0-1, row by row) of a backdrop on a canvas.
struct BackdropPlanes {
    std::vector<float> back;   ///< parts behind the front, drawn darker (folds, a second strip)
    std::vector<float> front;  ///< the shape itself
    std::vector<float> border; ///< the line along the front's edge (if bordered)
};

/// How far (pixels) the backdrop reaches beyond its boxes at most: bumps, torn ends, tails, tilt
/// and the soft edge.
[[nodiscard]] double backdrop_reach(const Backdrop& backdrop, std::span<const ShapeBox> boxes,
                                    double line_height);

/// The backdrop drawn around each box (one per line, or one around all) on a canvas of
/// width x height pixels; `line_height` in pixels sets the size of its details, `position` the
/// direction of a speech bubble's tip. `svg`: the drawing of an SVG backdrop (else unused).
[[nodiscard]] BackdropPlanes draw_backdrop(const Backdrop& backdrop,
                                           std::span<const ShapeBox> boxes, int width,
                                           int height, double line_height,
                                           TitlePosition position, const SvgShape* svg);

/// Box blur, three passes (close to a Gaussian), `radius` in pixels per pass.
void blur(std::vector<float>& plane, int width, int height, int radius);

} // namespace ttrally::highlights
