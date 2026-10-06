// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/media_info.hpp"

#include <filesystem>

namespace ttrally::media {

/// Port: reads container and stream properties. Throws MediaError if the file cannot be opened.
class MediaProbe {
  public:
    virtual ~MediaProbe() = default;
    [[nodiscard]] virtual MediaInfo probe(const std::filesystem::path& path) = 0;
};

} // namespace ttrally::media
