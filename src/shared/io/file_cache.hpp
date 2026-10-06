// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ttrally::io {

/// 64-bit FNV-1a hash.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view data) noexcept;

/// Cache key for data derived from a file: a hash of the absolute path, file size, modification
/// time and a purpose string (e.g. "audio-mono-8000"). Any change to the file changes the key.
[[nodiscard]] std::string cache_key(const std::filesystem::path& file, std::string_view purpose);

/// Writes a file via a temporary name and a final rename, so readers never see partial files.
template <typename WriteFn>
void write_file_atomically(const std::filesystem::path& path, WriteFn&& write) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary);
        if (!out) {
            throw std::runtime_error("cannot write " + temporary.string());
        }
        write(out);
        if (!out) {
            throw std::runtime_error("error while writing " + temporary.string());
        }
    }
    std::filesystem::rename(temporary, path);
}

} // namespace ttrally::io
