// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>

namespace ttrally {

/// Exact rational number, e.g. a frame rate of 60000/1001 or a stream time base of 1/90000.
struct Rational {
    std::int64_t num = 0;
    std::int64_t den = 1;

    [[nodiscard]] constexpr double value() const noexcept {
        return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
    }

    [[nodiscard]] constexpr bool positive() const noexcept { return num > 0 && den > 0; }
};

} // namespace ttrally
