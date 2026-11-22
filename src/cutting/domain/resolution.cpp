// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/domain/resolution.hpp"

#include <string>

namespace ttrally::cutting {

int output_height(int source_height, int max_height) noexcept {
    const int height =
        max_height > 0 && source_height > max_height ? max_height : source_height;
    return height & ~1;
}

std::filesystem::path with_resolution(const std::filesystem::path& file, int height) {
    return file.parent_path() /
           (file.stem().string() + "-" + std::to_string(height) + "p" +
            file.extension().string());
}

} // namespace ttrally::cutting
