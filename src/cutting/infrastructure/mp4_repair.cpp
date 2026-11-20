// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/mp4_repair.hpp"

#include "cutting/infrastructure/ffmpeg_encoding.hpp"
#include "media/application/media_error.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace ttrally::cutting {

namespace {

using media::MediaError;
namespace ff = media::ff;

constexpr std::uint32_t kHvc1 = MKTAG('h', 'v', 'c', '1');
constexpr const char* kRepairSuffix = ".repairing.mp4";

std::span<const std::uint8_t> bytes_of(const std::uint8_t* data, int size) {
    return {data, static_cast<std::size_t>(std::max(0, size))};
}

/// NAL length size meaning "the packets are Annex B" (files whose header is Annex B).
constexpr int kAnnexBPackets = 0;

/// The NAL units of an MP4 video packet. Whether it is length-prefixed cannot be told from its
/// bytes (a length 00 00 01 xx looks like a start code), only from the header.
std::vector<std::span<const std::uint8_t>> units_of(std::span<const std::uint8_t> packet,
                                                    int length_size) {
    return length_size == kAnnexBPackets ? annexb_units(packet)
                                         : length_prefixed_units(packet, length_size);
}

/// The header box's parameter sets and NAL length size (an Annex B header: kAnnexBPackets).
ConfigRecord header_of(const AVCodecParameters& parameters, NalCodec codec) {
    const auto header = bytes_of(parameters.extradata, parameters.extradata_size);
    if (header.empty()) {
        return {};
    }
    if (is_annexb(header)) {
        return {.parameter_sets = parameter_sets(annexb_units(header), codec),
                .length_size = kAnnexBPackets};
    }
    try {
        return parse_config_record(header, codec);
    } catch (const std::invalid_argument&) {
        return {}; // a broken header box: does not match the stream
    }
}

int video_stream_of(AVFormatContext* format, const std::filesystem::path& file) {
    const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        throw MediaError(file.string() + " has no video stream");
    }
    return index;
}

/// A copy of the file in which the video packets are Annex B, so that the output builds the
/// header box from their parameter sets.
void rewrite(const std::filesystem::path& file, const std::filesystem::path& copy) {
    const auto input = ff::open_input(file);
    const int video = video_stream_of(input.get(), file);
    const AVCodecParameters& video_parameters = *input->streams[video]->codecpar;
    const NalCodec codec =
        video_parameters.codec_id == AV_CODEC_ID_HEVC ? NalCodec::H265 : NalCodec::H264;
    const int length_size = header_of(video_parameters, codec).length_size;

    Output output(copy);
    std::vector<AVStream*> streams(input->nb_streams, nullptr);
    for (unsigned i = 0; i < input->nb_streams; ++i) {
        const AVStream* in = input->streams[i];
        const AVMediaType type = in->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
            input->streams[i]->discard = AVDISCARD_ALL;
            continue;
        }
        if (type == AVMEDIA_TYPE_VIDEO && static_cast<int>(i) != video) {
            input->streams[i]->discard = AVDISCARD_ALL;
            continue;
        }
        AVStream* out = output.add_stream();
        ff::check(avcodec_parameters_copy(out->codecpar, in->codecpar), "stream parameters");
        out->codecpar->codec_tag = 0;
        out->time_base = in->time_base;
        out->disposition = in->disposition;
        av_dict_copy(&out->metadata, in->metadata, 0);
        if (static_cast<int>(i) == video) {
            output.take_parameter_sets_from_stream(out);
        }
        streams[i] = out;
    }
    av_dict_copy(&output.context()->metadata, input->metadata, 0);
    output.open();

    auto packet = ff::make_packet();
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        const auto index = static_cast<std::size_t>(packet->stream_index);
        AVStream* out = index < streams.size() ? streams[index] : nullptr;
        if (out == nullptr) {
            av_packet_unref(packet.get());
            continue;
        }
        if (packet->stream_index == video) {
            const Bytes annexb =
                to_annexb(units_of(bytes_of(packet->data, packet->size), length_size));
            auto converted = ff::make_packet();
            ff::check(av_new_packet(converted.get(), static_cast<int>(annexb.size())),
                      "video packet");
            std::ranges::copy(annexb, converted->data);
            ff::check(av_packet_copy_props(converted.get(), packet.get()), "packet properties");
            av_packet_unref(packet.get());
            av_packet_move_ref(packet.get(), converted.get());
        }
        av_packet_rescale_ts(packet.get(), input->streams[index]->time_base, out->time_base);
        packet->stream_index = out->index;
        packet->pos = -1;
        output.write(*packet);
    }
    output.finish();
}

/// The packets of a file in order: per stream, the video packets as their NAL units without
/// parameter sets (which the repair moves into the header), the others as their bytes.
class PacketContent {
  public:
    explicit PacketContent(const std::filesystem::path& file)
        : input_(ff::open_input(file)), video_(video_stream_of(input_.get(), file)) {
        const AVCodecParameters& parameters = *input_->streams[video_]->codecpar;
        codec_ = parameters.codec_id == AV_CODEC_ID_HEVC ? NalCodec::H265 : NalCodec::H264;
        length_size_ = header_of(parameters, codec_).length_size;
    }

    /// The next packet of the given media type, or nullopt at the end.
    std::optional<std::vector<Bytes>> next(AVMediaType type) {
        auto packet = ff::make_packet();
        while (av_read_frame(input_.get(), packet.get()) >= 0) {
            const AVStream* stream = input_->streams[packet->stream_index];
            if (stream->codecpar->codec_type != type ||
                (type == AVMEDIA_TYPE_VIDEO && packet->stream_index != video_)) {
                av_packet_unref(packet.get());
                continue;
            }
            const auto data = bytes_of(packet->data, packet->size);
            std::vector<Bytes> content;
            if (type != AVMEDIA_TYPE_VIDEO) {
                content.emplace_back(data.begin(), data.end());
                return content;
            }
            const auto units = units_of(data, length_size_);
            const std::vector<Bytes> sets = parameter_sets(units, codec_);
            for (const auto unit : units) {
                Bytes bytes(unit.begin(), unit.end());
                if (std::ranges::find(sets, bytes) == sets.end()) {
                    content.push_back(std::move(bytes));
                }
            }
            return content;
        }
        return std::nullopt;
    }

    void rewind() {
        ff::check(av_seek_frame(input_.get(), -1, 0, AVSEEK_FLAG_BACKWARD), "rewinding");
    }

  private:
    ff::FormatPtr input_;
    int video_;
    NalCodec codec_ = NalCodec::H265;
    int length_size_ = 4;
};

/// Whether the copy holds the same video pictures and sound as the original, packet by packet.
bool same_content(const std::filesystem::path& original, const std::filesystem::path& copy) {
    PacketContent a(original);
    PacketContent b(copy);
    for (const AVMediaType type : {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO}) {
        a.rewind();
        b.rewind();
        for (;;) {
            const auto from_a = a.next(type);
            const auto from_b = b.next(type);
            if (from_a != from_b) {
                return false;
            }
            if (!from_a) {
                break;
            }
        }
    }
    return true;
}

} // namespace

Mp4Check check_mp4(const std::filesystem::path& file) {
    const auto input = ff::open_input(file);
    const int video = video_stream_of(input.get(), file);
    const AVCodecParameters& parameters = *input->streams[video]->codecpar;
    Mp4Check check;
    check.h26x =
        parameters.codec_id == AV_CODEC_ID_H264 || parameters.codec_id == AV_CODEC_ID_HEVC;
    if (!check.h26x) {
        return check;
    }
    check.codec = parameters.codec_id == AV_CODEC_ID_HEVC ? NalCodec::H265 : NalCodec::H264;
    check.tagged_hvc1 = parameters.codec_tag == kHvc1;
    const ConfigRecord header = header_of(parameters, check.codec);
    check.header_sets = header.parameter_sets;
    auto packet = ff::make_packet();
    while (av_read_frame(input.get(), packet.get()) >= 0) {
        if (packet->stream_index != video) {
            av_packet_unref(packet.get());
            continue;
        }
        try {
            check.stream_sets = parameter_sets(
                units_of(bytes_of(packet->data, packet->size), header.length_size), check.codec);
        } catch (const std::invalid_argument&) {
            // packets that do not split by the header's length size: header and stream disagree
            check.header_sets.clear();
        }
        break;
    }
    return check;
}

RepairOutcome repair_mp4(const std::filesystem::path& file) {
    const Mp4Check before = check_mp4(file);
    if (before.fine()) {
        return RepairOutcome::AlreadyFine;
    }
    if (!before.h26x || before.stream_sets.empty()) {
        return RepairOutcome::NotRepairable;
    }
    std::filesystem::path copy = file;
    copy += kRepairSuffix;
    try {
        rewrite(file, copy);
        const Mp4Check after = check_mp4(copy);
        if (!after.fine() || after.header_sets != before.stream_sets) {
            throw MediaError("the repaired copy of " + file.string() + " is still faulty");
        }
        if (!same_content(file, copy)) {
            throw MediaError("the repaired copy of " + file.string() +
                             " differs from the original; the original was kept");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(copy, ignored);
        throw;
    }
    std::filesystem::rename(copy, file);
    return RepairOutcome::Repaired;
}

} // namespace ttrally::cutting
