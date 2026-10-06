// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ttrally::io {

/// N-dimensional array read from a .npy file (C order).
template <typename T> struct NpyArray {
    std::vector<std::size_t> shape;
    std::vector<T> data;
};

/// Writes a C-ordered array as NPY format version 1.0.
/// Supported element types: float, double, std::int64_t.
template <typename T>
void write_npy(const std::filesystem::path& path, std::span<const T> data,
               std::span<const std::size_t> shape);

/// Reads an NPY file (format version 1.0 or 2.0, little endian, C order) whose element type
/// matches T exactly.
template <typename T> NpyArray<T> read_npy(const std::filesystem::path& path);

extern template void write_npy<float>(const std::filesystem::path&, std::span<const float>,
                                      std::span<const std::size_t>);
extern template void write_npy<double>(const std::filesystem::path&, std::span<const double>,
                                       std::span<const std::size_t>);
extern template void write_npy<std::int64_t>(const std::filesystem::path&,
                                             std::span<const std::int64_t>,
                                             std::span<const std::size_t>);
extern template NpyArray<float> read_npy<float>(const std::filesystem::path&);
extern template NpyArray<double> read_npy<double>(const std::filesystem::path&);
extern template NpyArray<std::int64_t> read_npy<std::int64_t>(const std::filesystem::path&);

} // namespace ttrally::io
