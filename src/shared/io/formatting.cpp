// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/io/formatting.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace ttrally::io {

std::string format_seconds(double seconds) { return std::format("{:.6f}", seconds); }

std::string format_fps(Rational fps) {
    std::string text = std::format("{:.5f}", fps.value());
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::string format_clock(double seconds) {
    if (std::isnan(seconds)) {
        return "–";
    }
    constexpr long long kMillisPerHour = 3'600'000;
    constexpr long long kMillisPerMinute = 60'000;
    constexpr long long kMillisPerSecond = 1'000;
    const bool negative = seconds < 0.0;
    const auto millis = static_cast<long long>(std::llround(std::abs(seconds) * 1000.0));
    return std::format("{}{}:{:02}:{:02}.{:03}", negative ? "-" : "", millis / kMillisPerHour,
                       millis / kMillisPerMinute % 60, millis / kMillisPerSecond % 60,
                       millis % kMillisPerSecond);
}

std::string format_elapsed(double seconds) {
    constexpr long long kHour = 3600;
    constexpr long long kMinute = 60;
    const long long total = std::llround(std::max(0.0, seconds));
    if (total < kMinute) {
        return std::format("{} s", total);
    }
    if (total < kHour) {
        return std::format("{} min {:02} s", total / kMinute, total % kMinute);
    }
    return std::format("{} h {:02} min {:02} s", total / kHour, total / kMinute % kMinute,
                       total % kMinute);
}

std::string format_size(double bytes) {
    constexpr double kMegabyte = 1e6;
    constexpr double kGigabyte = 1e9;
    return bytes >= kGigabyte ? std::format("{:.1f} GB", bytes / kGigabyte)
                              : std::format("{:.0f} MB", bytes / kMegabyte);
}

} // namespace ttrally::io
