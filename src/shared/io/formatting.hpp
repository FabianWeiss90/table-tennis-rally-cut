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

/// How long something took, rounded to whole seconds: "45 s", "12 min 05 s", "1 h 02 min 05 s".
[[nodiscard]] std::string format_elapsed(double seconds);

/// A file size in GB (from 1 GB) or MB, e.g. "8.4 GB", "350 MB".
[[nodiscard]] std::string format_size(double bytes);

} // namespace ttrally::io
