// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdexcept>

namespace ttrally::media {

/// A media file cannot be opened, has no suitable stream, or cannot be decoded.
class MediaError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

} // namespace ttrally::media
