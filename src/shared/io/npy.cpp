// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/io/npy.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ttrally::io {

static_assert(std::endian::native == std::endian::little, "NPY io assumes a little-endian host");

namespace {

constexpr std::array<char, 6> kMagic{'\x93', 'N', 'U', 'M', 'P', 'Y'};

template <typename T> constexpr std::string_view descr();
template <> constexpr std::string_view descr<float>() { return "<f4"; }
template <> constexpr std::string_view descr<double>() { return "<f8"; }
template <> constexpr std::string_view descr<std::int64_t>() { return "<i8"; }

std::size_t element_count(std::span<const std::size_t> shape) {
    return std::accumulate(shape.begin(), shape.end(), std::size_t{1}, std::multiplies<>{});
}

std::string shape_to_string(std::span<const std::size_t> shape) {
    // Python tuple syntax: "()", "(5,)", "(2, 3)"
    std::string text = "(";
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += std::to_string(shape[i]);
    }
    if (shape.size() == 1) {
        text += ',';
    }
    text += ')';
    return text;
}

std::string_view header_value(std::string_view header, std::string_view key) {
    const std::string quoted = "'" + std::string(key) + "'";
    const auto pos = header.find(quoted);
    if (pos == std::string_view::npos) {
        throw std::runtime_error("NPY header lacks key " + quoted);
    }
    const auto colon = header.find(':', pos + quoted.size());
    if (colon == std::string_view::npos) {
        throw std::runtime_error("malformed NPY header");
    }
    std::size_t begin = header.find_first_not_of(' ', colon + 1);
    if (begin == std::string_view::npos) {
        throw std::runtime_error("malformed NPY header");
    }
    std::size_t end = begin;
    if (header[begin] == '(') {
        end = header.find(')', begin);
        if (end == std::string_view::npos) {
            throw std::runtime_error("malformed NPY shape");
        }
        ++end;
    } else if (header[begin] == '\'') {
        end = header.find('\'', begin + 1);
        if (end == std::string_view::npos) {
            throw std::runtime_error("malformed NPY header");
        }
        ++end;
    } else {
        end = header.find_first_of(",}", begin);
    }
    return header.substr(begin, end - begin);
}

std::vector<std::size_t> parse_shape(std::string_view text) {
    std::vector<std::size_t> shape;
    std::size_t value = 0;
    bool in_number = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<std::size_t>(c - '0');
            in_number = true;
        } else if (in_number) {
            shape.push_back(value);
            value = 0;
            in_number = false;
        }
    }
    return shape;
}

} // namespace

template <typename T>
void write_npy(const std::filesystem::path& path, std::span<const T> data,
               std::span<const std::size_t> shape) {
    if (element_count(shape) != data.size()) {
        throw std::invalid_argument("NPY shape does not match the number of elements");
    }
    std::string header = "{'descr': '" + std::string(descr<T>()) +
                         "', 'fortran_order': False, 'shape': " + shape_to_string(shape) + ", }";
    // Pad with spaces and a final newline so that the data starts at a multiple of 64 bytes.
    constexpr std::size_t kPrefix = kMagic.size() + 2 + 2;
    const std::size_t total = kPrefix + header.size() + 1;
    header.append((64 - total % 64) % 64, ' ');
    header += '\n';
    if (header.size() > 0xFFFF) {
        throw std::invalid_argument("NPY header too long");
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
    out.write(kMagic.data(), kMagic.size());
    const std::array<char, 2> version{1, 0};
    out.write(version.data(), version.size());
    const auto header_len = static_cast<std::uint16_t>(header.size());
    const std::array<char, 2> len_bytes{static_cast<char>(header_len & 0xFF),
                                        static_cast<char>(header_len >> 8)};
    out.write(len_bytes.data(), len_bytes.size());
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size_bytes()));
    if (!out) {
        throw std::runtime_error("error while writing " + path.string());
    }
}

template <typename T> NpyArray<T> read_npy(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::array<char, 8> prefix{};
    in.read(prefix.data(), prefix.size());
    if (!in || !std::equal(kMagic.begin(), kMagic.end(), prefix.begin())) {
        throw std::runtime_error(path.string() + " is not an NPY file");
    }
    const auto major = static_cast<unsigned char>(prefix[6]);
    std::size_t header_len = 0;
    if (major == 1) {
        std::array<unsigned char, 2> len{};
        in.read(reinterpret_cast<char*>(len.data()), len.size());
        header_len = static_cast<std::size_t>(len[0]) | (static_cast<std::size_t>(len[1]) << 8U);
    } else if (major == 2 || major == 3) {
        std::array<unsigned char, 4> len{};
        in.read(reinterpret_cast<char*>(len.data()), len.size());
        for (std::size_t i = 0; i < len.size(); ++i) {
            header_len |= static_cast<std::size_t>(len[i]) << (8U * i);
        }
    } else {
        throw std::runtime_error("unsupported NPY version in " + path.string());
    }
    std::string header(header_len, '\0');
    in.read(header.data(), static_cast<std::streamsize>(header_len));
    if (!in) {
        throw std::runtime_error("truncated NPY header in " + path.string());
    }

    const std::string_view type = header_value(header, "descr");
    if (type != "'" + std::string(descr<T>()) + "'") {
        throw std::runtime_error("unexpected NPY element type " + std::string(type) + " in " +
                                 path.string());
    }
    if (header_value(header, "fortran_order") != "False") {
        throw std::runtime_error("Fortran-ordered NPY files are not supported");
    }

    NpyArray<T> array;
    array.shape = parse_shape(header_value(header, "shape"));
    array.data.resize(element_count(array.shape));
    in.read(reinterpret_cast<char*>(array.data.data()),
            static_cast<std::streamsize>(array.data.size() * sizeof(T)));
    if (!in) {
        throw std::runtime_error("truncated NPY data in " + path.string());
    }
    return array;
}

template void write_npy<float>(const std::filesystem::path&, std::span<const float>,
                               std::span<const std::size_t>);
template void write_npy<double>(const std::filesystem::path&, std::span<const double>,
                                std::span<const std::size_t>);
template void write_npy<std::int64_t>(const std::filesystem::path&, std::span<const std::int64_t>,
                                      std::span<const std::size_t>);
template NpyArray<float> read_npy<float>(const std::filesystem::path&);
template NpyArray<double> read_npy<double>(const std::filesystem::path&);
template NpyArray<std::int64_t> read_npy<std::int64_t>(const std::filesystem::path&);

} // namespace ttrally::io
