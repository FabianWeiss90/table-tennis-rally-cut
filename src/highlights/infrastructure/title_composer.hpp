// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "highlights/application/title_fonts.hpp"
#include "highlights/infrastructure/title_painter.hpp"

namespace ttrally::highlights {

class SvgShape;

/// The whole title, bottom to top: shadow, backdrop (parts behind darker, then its border), glow,
/// the letters' border and the letters; cut to what is drawn. `svg`: the drawing of an SVG
/// backdrop (nullptr: no backdrop is drawn for one).
[[nodiscard]] TitleImage compose_title(const TitleMask& letters, const Title& title,
                                       int line_height, const SvgShape* svg);

} // namespace ttrally::highlights
