// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The parameter sets of H.264 and H.265 video (VPS, SPS, PPS: picture size, profile, coding
// tools), which a decoder needs before the first picture. MP4 keeps them in the file header (the
// avcC/hvcC box); encoders may also repeat them in the stream before each keyframe. The VAAPI
// encoder on Mesa (AMD) writes a header copy that differs from the stream copy it encodes with,
// so the header is built from the stream instead (see Output).

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ttrally::cutting {

enum class NalCodec { H264, H265 };

using Bytes = std::vector<std::uint8_t>;

/// The NAL units of an Annex B stream (each after a start code 00 00 01 or 00 00 00 01),
/// without start codes and trailing zero bytes.
[[nodiscard]] std::vector<std::span<const std::uint8_t>>
annexb_units(std::span<const std::uint8_t> data);

/// Whether the data starts with an Annex B start code.
[[nodiscard]] bool is_annexb(std::span<const std::uint8_t> data);

/// The parameter sets (H.265: VPS, SPS, PPS; H.264: SPS, PPS) among the NAL units, each once,
/// in the order they appear.
[[nodiscard]] std::vector<Bytes> parameter_sets(
    const std::vector<std::span<const std::uint8_t>>& units, NalCodec codec);

/// The NAL units of an MP4 sample (each after its size in `length_size` bytes, big endian).
/// Throws std::invalid_argument if a size runs past the end.
[[nodiscard]] std::vector<std::span<const std::uint8_t>>
length_prefixed_units(std::span<const std::uint8_t> data, int length_size);

/// NAL units as Annex B (each after 00 00 00 01).
[[nodiscard]] Bytes to_annexb(const std::vector<std::span<const std::uint8_t>>& units);
[[nodiscard]] Bytes to_annexb(const std::vector<Bytes>& units);

/// The parameter sets in an MP4 header box (avcC or hvcC) and the size of the NAL length
/// fields of its samples. Throws std::invalid_argument if the box is cut short.
struct ConfigRecord {
    std::vector<Bytes> parameter_sets;
    int length_size = 4;
};
[[nodiscard]] ConfigRecord parse_config_record(std::span<const std::uint8_t> record,
                                               NalCodec codec);

} // namespace ttrally::cutting
