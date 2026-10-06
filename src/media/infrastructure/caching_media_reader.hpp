// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/media_reader.hpp"

#include <filesystem>

namespace ttrally::media {

/// Decorator that caches decoded audio (raw float32) and frame timestamps on disk, keyed by
/// file path, size and modification time. Only the parts missing from the cache are read.
class CachingMediaReader final : public MediaReader {
  public:
    CachingMediaReader(MediaReader& inner, std::filesystem::path cache_dir)
        : inner_(inner), cache_dir_(std::move(cache_dir)) {}

    [[nodiscard]] MediaContent read(const std::filesystem::path& path,
                                    const ReadRequest& request) override;

  private:
    MediaReader& inner_;
    std::filesystem::path cache_dir_;
};

} // namespace ttrally::media
