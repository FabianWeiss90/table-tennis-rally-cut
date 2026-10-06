// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <string_view>

namespace ttrally {

/// Version of the ttrally tool, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

} // namespace ttrally
