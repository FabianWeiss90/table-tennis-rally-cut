// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>
#include <vector>

namespace ttrally::media {

/// 8-bit greyscale image, row-major.
struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

} // namespace ttrally::media
