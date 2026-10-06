// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "shared/kernel/rational.hpp"

#include <string>

namespace ttrally::io {

/// Seconds with fixed microsecond precision, e.g. "12.345678".
[[nodiscard]] std::string format_seconds(double seconds);

/// Frame rate as a short decimal, e.g. "60", "59.94006", "25".
[[nodiscard]] std::string format_fps(Rational fps);

/// Clock notation h:mm:ss.mmm, e.g. "0:01:02.345"; "–" for NaN.
[[nodiscard]] std::string format_clock(double seconds);

} // namespace ttrally::io
