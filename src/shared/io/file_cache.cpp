// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/io/file_cache.hpp"

#include <cstdint>
#include <format>

namespace ttrally::io {

std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string cache_key(const std::filesystem::path& file, std::string_view purpose) {
    const auto absolute = std::filesystem::absolute(file);
    const auto u8 = absolute.generic_u8string();
    const std::string identity =
        std::format("{}|{}|{}|{}", std::string(u8.begin(), u8.end()),
                    std::filesystem::file_size(file),
                    std::filesystem::last_write_time(file).time_since_epoch().count(), purpose);
    return std::format("{:016x}", fnv1a64(identity));
}

} // namespace ttrally::io
