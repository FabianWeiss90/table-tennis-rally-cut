// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Splitting H.264/H.265 data into NAL units and finding the parameter sets, as the MP4 output
// and the repair of earlier files do.

#include "cutting/infrastructure/parameter_sets.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace ttrally::cutting;

namespace {

// H.265 NAL headers: type in bits 1-6 of the first byte
const Bytes kVps{0x40, 0x01, 0x0C};
const Bytes kSps{0x42, 0x01, 0x01};
const Bytes kPps{0x44, 0x01, 0xC0};
const Bytes kSlice{0x26, 0x01, 0xAF, 0x00, 0x00, 0x03, 0x01}; // IDR with emulation prevention

std::vector<Bytes> as_bytes(const std::vector<std::span<const std::uint8_t>>& units) {
    std::vector<Bytes> bytes;
    for (const auto unit : units) {
        bytes.emplace_back(unit.begin(), unit.end());
    }
    return bytes;
}

} // namespace

TEST_CASE("Annex B data splits at 3- and 4-byte start codes") {
    const Bytes data{0, 0, 0, 1, 0x40, 0x01, 0x0C, 0, 0, 1, 0x42, 0x01, 0x01, 0, 0, 0, 0, 1,
                     0x26, 0x01, 0xAF};
    CHECK(as_bytes(annexb_units(data)) ==
          std::vector<Bytes>{kVps, kSps, {0x26, 0x01, 0xAF}});
    CHECK(is_annexb(data));
}

TEST_CASE("the parameter sets are found once each, in order") {
    const Bytes stream = to_annexb(std::vector<Bytes>{kVps, kSps, kPps, kSlice, kVps, kSps});
    CHECK(parameter_sets(annexb_units(stream), NalCodec::H265) ==
          std::vector<Bytes>{kVps, kSps, kPps});
    // H.264: SPS (7) and PPS (8)
    const Bytes h264_sps{0x67, 0x64}, h264_pps{0x68, 0xEE}, h264_idr{0x65, 0x88};
    const Bytes h264 = to_annexb(std::vector<Bytes>{h264_sps, h264_pps, h264_idr});
    CHECK(parameter_sets(annexb_units(h264), NalCodec::H264) ==
          std::vector<Bytes>{h264_sps, h264_pps});
}

TEST_CASE("length-prefixed samples split by their sizes, also when a size looks like a start "
          "code") {
    // A NAL of 0x18F bytes: its size 00 00 01 8F begins like an Annex B start code
    Bytes nal(0x18F, 0x5A);
    nal[0] = 0x02;
    Bytes sample{0x00, 0x00, 0x01, 0x8F};
    sample.insert(sample.end(), nal.begin(), nal.end());
    const auto units = length_prefixed_units(sample, 4);
    REQUIRE(units.size() == 1);
    CHECK(Bytes(units[0].begin(), units[0].end()) == nal);
    CHECK_THROWS_AS(length_prefixed_units(Bytes{0, 0, 0, 9, 1, 2}, 4), std::invalid_argument);
}

TEST_CASE("the parameter sets are read from hvcC and avcC boxes") {
    Bytes hvcc(22, 0);
    hvcc[21] = 0x03; // 4-byte NAL sizes
    hvcc.push_back(3); // three arrays
    for (const Bytes* set : {&kVps, &kSps, &kPps}) {
        hvcc.push_back((*set)[0] >> 1);
        hvcc.insert(hvcc.end(), {0x00, 0x01, 0x00, static_cast<std::uint8_t>(set->size())});
        hvcc.insert(hvcc.end(), set->begin(), set->end());
    }
    const ConfigRecord h265 = parse_config_record(hvcc, NalCodec::H265);
    CHECK(h265.length_size == 4);
    CHECK(h265.parameter_sets == std::vector<Bytes>{kVps, kSps, kPps});
    CHECK_THROWS_AS(parse_config_record(Bytes(hvcc.begin(), hvcc.end() - 2), NalCodec::H265),
                    std::invalid_argument);

    const Bytes avcc{0x01, 0x64, 0x00, 0x28, 0xFF, 0xE1, 0x00, 0x02, 0x67, 0x64,
                     0x01, 0x00, 0x02, 0x68, 0xEE};
    const ConfigRecord h264 = parse_config_record(avcc, NalCodec::H264);
    CHECK(h264.length_size == 4);
    CHECK(h264.parameter_sets == std::vector<Bytes>{{0x67, 0x64}, {0x68, 0xEE}});
}
