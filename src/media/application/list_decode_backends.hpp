// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/application/decode_backend_probe.hpp"

#include <vector>

namespace ttrally::media {

struct DecodeBackendOverview {
    std::vector<BackendStatus> hardware; ///< In probe order
    DecodeBackend automatic = DecodeBackend::Cpu;
};

/// Use case behind `ttrally devices`: which decode backends exist and which one `auto` picks.
class ListDecodeBackends {
  public:
    explicit ListDecodeBackends(DecodeBackendProbe& probe) : probe_(probe) {}
    [[nodiscard]] DecodeBackendOverview execute();

  private:
    DecodeBackendProbe& probe_;
};

} // namespace ttrally::media
