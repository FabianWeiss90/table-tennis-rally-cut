// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/parameter_sets.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace ttrally::cutting {

namespace {

constexpr int kH265NalTypeShift = 1;
constexpr std::uint8_t kH265NalTypeMask = 0x3F;
constexpr std::uint8_t kH264NalTypeMask = 0x1F;
constexpr int kH265Vps = 32;
constexpr int kH265Pps = 34;
constexpr int kH264Sps = 7;
constexpr int kH264Pps = 8;
constexpr std::uint8_t kLengthSizeMask = 0x03;
constexpr std::size_t kAvcCLengthByte = 4;
constexpr std::size_t kAvcCSpsCountByte = 5;
constexpr std::uint8_t kAvcCSpsCountMask = 0x1F;
constexpr std::size_t kHvcCLengthByte = 21;
constexpr std::size_t kHvcCArrayCountByte = 22;
constexpr std::array<std::uint8_t, 4> kStartCode{0, 0, 0, 1};
constexpr std::size_t kNoUnit = std::numeric_limits<std::size_t>::max();

int nal_type(std::span<const std::uint8_t> unit, NalCodec codec) {
    if (unit.empty()) {
        return -1;
    }
    return codec == NalCodec::H265 ? (unit[0] >> kH265NalTypeShift) & kH265NalTypeMask
                                   : unit[0] & kH264NalTypeMask;
}

bool is_parameter_set(int type, NalCodec codec) {
    return codec == NalCodec::H265 ? type >= kH265Vps && type <= kH265Pps
                                   : type == kH264Sps || type == kH264Pps;
}

/// Reads a record field by field, refusing to read past its end.
class Reader {
  public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint8_t byte_at(std::size_t at) const {
        need(at + 1);
        return data_[at];
    }
    void seek(std::size_t at) {
        need(at);
        pos_ = at;
    }
    std::uint8_t u8() {
        need(pos_ + 1);
        return data_[pos_++];
    }
    std::uint16_t u16() {
        const auto high = u8();
        return static_cast<std::uint16_t>((high << 8) | u8());
    }
    Bytes bytes(std::size_t count) {
        need(pos_ + count);
        Bytes out(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
                  data_.begin() + static_cast<std::ptrdiff_t>(pos_ + count));
        pos_ += count;
        return out;
    }

  private:
    void need(std::size_t end) const {
        if (end > data_.size()) {
            throw std::invalid_argument("the MP4 header box of the video is cut short");
        }
    }
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

} // namespace

std::vector<std::span<const std::uint8_t>> annexb_units(std::span<const std::uint8_t> data) {
    std::vector<std::span<const std::uint8_t>> units;
    std::size_t start = kNoUnit;
    const auto close = [&](std::size_t end) {
        if (start == kNoUnit) {
            return;
        }
        while (end > start && data[end - 1] == 0) {
            --end;
        }
        if (end > start) {
            units.push_back(data.subspan(start, end - start));
        }
    };
    for (std::size_t i = 0; i + 2 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            close(i);
            start = i + 3;
            i += 2;
        }
    }
    close(data.size());
    return units;
}

bool is_annexb(std::span<const std::uint8_t> data) {
    return (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
           (data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

std::vector<Bytes> parameter_sets(const std::vector<std::span<const std::uint8_t>>& units,
                                  NalCodec codec) {
    std::vector<Bytes> sets;
    for (const auto unit : units) {
        if (!is_parameter_set(nal_type(unit, codec), codec)) {
            continue;
        }
        Bytes set(unit.begin(), unit.end());
        if (std::ranges::find(sets, set) == sets.end()) {
            sets.push_back(std::move(set));
        }
    }
    return sets;
}

std::vector<std::span<const std::uint8_t>> length_prefixed_units(std::span<const std::uint8_t> data,
                                                                 int length_size) {
    std::vector<std::span<const std::uint8_t>> units;
    const auto size_bytes = static_cast<std::size_t>(length_size);
    std::size_t pos = 0;
    while (pos < data.size()) {
        if (pos + size_bytes > data.size()) {
            throw std::invalid_argument("a video packet ends inside a NAL size field");
        }
        std::size_t size = 0;
        for (std::size_t k = 0; k < size_bytes; ++k) {
            size = (size << 8) | data[pos + k];
        }
        pos += size_bytes;
        if (size > data.size() - pos) {
            throw std::invalid_argument("a NAL unit runs past the end of its video packet");
        }
        units.push_back(data.subspan(pos, size));
        pos += size;
    }
    return units;
}

Bytes to_annexb(const std::vector<std::span<const std::uint8_t>>& units) {
    Bytes out;
    for (const auto unit : units) {
        out.insert(out.end(), kStartCode.begin(), kStartCode.end());
        out.insert(out.end(), unit.begin(), unit.end());
    }
    return out;
}

Bytes to_annexb(const std::vector<Bytes>& units) {
    std::vector<std::span<const std::uint8_t>> spans(units.begin(), units.end());
    return to_annexb(spans);
}

ConfigRecord parse_config_record(std::span<const std::uint8_t> record, NalCodec codec) {
    ConfigRecord result;
    Reader reader(record);
    if (codec == NalCodec::H264) {
        result.length_size = (reader.byte_at(kAvcCLengthByte) & kLengthSizeMask) + 1;
        reader.seek(kAvcCSpsCountByte);
        const int sps_count = reader.u8() & kAvcCSpsCountMask;
        for (int i = 0; i < sps_count; ++i) {
            result.parameter_sets.push_back(reader.bytes(reader.u16()));
        }
        const int pps_count = reader.u8();
        for (int i = 0; i < pps_count; ++i) {
            result.parameter_sets.push_back(reader.bytes(reader.u16()));
        }
        return result;
    }
    result.length_size = (reader.byte_at(kHvcCLengthByte) & kLengthSizeMask) + 1;
    reader.seek(kHvcCArrayCountByte);
    const int arrays = reader.u8();
    for (int a = 0; a < arrays; ++a) {
        const int type = reader.u8() & kH265NalTypeMask;
        const int count = reader.u16();
        for (int i = 0; i < count; ++i) {
            Bytes unit = reader.bytes(reader.u16());
            if (is_parameter_set(type, codec)) {
                result.parameter_sets.push_back(std::move(unit));
            }
        }
    }
    return result;
}

} // namespace ttrally::cutting
