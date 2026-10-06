// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/media_probe.hpp"

namespace ttrally::media {

class FfmpegMediaProbe final : public MediaProbe {
  public:
    [[nodiscard]] MediaInfo probe(const std::filesystem::path& path) override;
};

} // namespace ttrally::media
