// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/decode_backend_probe.hpp"

namespace ttrally::media {

/// Probes FFmpeg hwaccel device types by trying to create a device for each.
class FfmpegDecodeBackendProbe final : public DecodeBackendProbe {
  public:
    [[nodiscard]] std::vector<BackendStatus> probe() override;
};

} // namespace ttrally::media
