// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/decode_backend.hpp"

#include <vector>

namespace ttrally::media {

/// Port: checks which hardware decode backends work on this machine, in the platform's probe
/// order (Linux: VAAPI, CUDA, Vulkan; Windows: D3D12VA, D3D11VA, CUDA).
class DecodeBackendProbe {
  public:
    virtual ~DecodeBackendProbe() = default;
    [[nodiscard]] virtual std::vector<BackendStatus> probe() = 0;
};

} // namespace ttrally::media
