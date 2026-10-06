// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/media_reader.hpp"

namespace ttrally::media {

/// Reads the requested data in one demuxing pass; audio is downmixed and resampled with
/// libswresample.
class FfmpegMediaReader final : public MediaReader {
  public:
    [[nodiscard]] MediaContent read(const std::filesystem::path& path,
                                    const ReadRequest& request) override;
};

} // namespace ttrally::media
