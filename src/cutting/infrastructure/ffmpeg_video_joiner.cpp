// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/ffmpeg_video_joiner.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"
#include "shared/io/formatting.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ttrally::cutting {

namespace ff = media::ff;
using media::MediaError;

namespace {

constexpr std::size_t kVideo = 0;
constexpr std::size_t kAudio = 1;
constexpr int kProgressSteps = 10; ///< messages per part
constexpr std::int64_t kNoTimestamp = std::numeric_limits<std::int64_t>::min();

/// A part opened for reading, with the streams that are copied.
struct Part {
    std::filesystem::path path;
    ff::FormatPtr format;
    std::array<int, 2> streams{-1, -1}; ///< video, audio (-1: none)

    [[nodiscard]] const AVCodecParameters& parameters(std::size_t kind) const {
        return *format->streams[streams[kind]]->codecpar;
    }
};

Part open_part(const std::filesystem::path& path) {
    Part part{path, ff::open_input(path), {-1, -1}};
    part.streams[kVideo] =
        av_find_best_stream(part.format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (part.streams[kVideo] < 0) {
        throw MediaError(path.string() + " has no video stream");
    }
    const int audio =
        av_find_best_stream(part.format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    part.streams[kAudio] = audio >= 0 ? audio : -1;
    return part;
}

bool same_extradata(const AVCodecParameters& a, const AVCodecParameters& b) {
    return a.extradata_size == b.extradata_size &&
           (a.extradata_size == 0 || std::memcmp(a.extradata, b.extradata,
                                                 static_cast<std::size_t>(a.extradata_size)) == 0);
}

/// What differs between a stream of the first part and of another; empty if they can follow
/// each other without re-encoding.
std::string difference(const AVCodecParameters& first, const AVCodecParameters& other) {
    if (first.codec_id != other.codec_id) {
        return std::format("codec {} instead of {}", avcodec_get_name(other.codec_id),
                           avcodec_get_name(first.codec_id));
    }
    if (first.codec_type == AVMEDIA_TYPE_VIDEO &&
        (first.width != other.width || first.height != other.height)) {
        return std::format("{}x{} instead of {}x{}", other.width, other.height, first.width,
                           first.height);
    }
    if (first.codec_type == AVMEDIA_TYPE_AUDIO &&
        (first.sample_rate != other.sample_rate ||
         first.ch_layout.nb_channels != other.ch_layout.nb_channels)) {
        return std::format("sound at {} Hz with {} channels instead of {} Hz with {}",
                           other.sample_rate, other.ch_layout.nb_channels, first.sample_rate,
                           first.ch_layout.nb_channels);
    }
    if (!same_extradata(first, other)) {
        return "other codec settings";
    }
    return {};
}

void check_compatible(const std::vector<Part>& parts) {
    const Part& first = parts.front();
    for (const Part& part : parts) {
        std::string problem = difference(first.parameters(kVideo), part.parameters(kVideo));
        if (problem.empty() && first.streams[kAudio] >= 0) {
            problem = part.streams[kAudio] < 0
                          ? std::string("no sound")
                          : difference(first.parameters(kAudio), part.parameters(kAudio));
        }
        if (!problem.empty()) {
            throw MediaError(std::format("{} cannot be appended to {} without re-encoding: {}",
                                         part.path.string(), first.path.filename().string(),
                                         problem));
        }
    }
}

/// The written file: in a temporary file next to the destination until it is complete.
class JoinedOutput {
  public:
    JoinedOutput(const std::filesystem::path& output, const Part& first)
        : output_(output),
          temporary_(output.parent_path() /
                     (output.stem().string() + ".merging" + output.extension().string())) {
        const AVOutputFormat* format =
            av_guess_format(nullptr, ff::path_to_utf8(output).c_str(), nullptr);
        if (format == nullptr) {
            throw MediaError(std::format("{}: unknown video format for the extension \"{}\"",
                                         output.string(), output.extension().string()));
        }
        AVFormatContext* context = nullptr;
        ff::check(avformat_alloc_output_context2(&context, format, nullptr,
                                                 ff::path_to_utf8(temporary_).c_str()),
                  "output");
        context_.reset(context);
        for (std::size_t kind : {kVideo, kAudio}) {
            if (first.streams[kind] >= 0) {
                add_stream(kind, *first.format->streams[first.streams[kind]]);
            }
        }
        if (!output.parent_path().empty()) {
            std::filesystem::create_directories(output.parent_path());
        }
        ff::check(avio_open(&context_->pb, ff::path_to_utf8(temporary_).c_str(), AVIO_FLAG_WRITE),
                  "opening " + temporary_.string());
        AVDictionary* options = nullptr;
        const std::string name = format->name;
        if (name.find("mp4") != std::string::npos || name.find("mov") != std::string::npos) {
            av_dict_set(&options, "movflags", "+faststart", 0); // playable while loading
        }
        const int written = avformat_write_header(context_.get(), &options);
        av_dict_free(&options);
        ff::check(written, "writing the header");
    }

    JoinedOutput(const JoinedOutput&) = delete;
    JoinedOutput& operator=(const JoinedOutput&) = delete;

    ~JoinedOutput() {
        if (!finished_) {
            close();
            std::error_code ignored;
            std::filesystem::remove(temporary_, ignored);
        }
    }

    /// Output stream of a kind (video, audio); nullptr if the parts have none.
    [[nodiscard]] AVStream* stream(std::size_t kind) const { return streams_[kind]; }

    /// Writes a packet whose timestamps are already in the output stream's time base, keeping
    /// each stream's decoding times increasing.
    void write(AVPacket& packet, std::size_t kind) {
        std::int64_t& last = last_dts_[kind];
        if (packet.dts != AV_NOPTS_VALUE) {
            if (last != kNoTimestamp && packet.dts <= last) {
                packet.dts = last + 1;
                if (packet.pts != AV_NOPTS_VALUE) {
                    packet.pts = std::max(packet.pts, packet.dts);
                }
            }
            last = packet.dts;
        }
        packet.stream_index = streams_[kind]->index;
        packet.pos = -1;
        ff::check(av_interleaved_write_frame(context_.get(), &packet), "writing");
    }

    void finish() {
        ff::check(av_write_trailer(context_.get()), "finishing the file");
        close();
        std::filesystem::rename(temporary_, output_);
        finished_ = true;
    }

  private:
    struct ContextFreer {
        void operator()(AVFormatContext* context) const noexcept {
            avformat_free_context(context);
        }
    };

    void add_stream(std::size_t kind, const AVStream& source) {
        AVStream* stream = avformat_new_stream(context_.get(), nullptr);
        if (stream == nullptr) {
            throw MediaError("cannot add a stream to the output");
        }
        ff::check(avcodec_parameters_copy(stream->codecpar, source.codecpar), "stream");
        // Keep the source's tag (e.g. hvc1, which Apple's players need) if the container knows
        // it for this codec, else let the muxer choose
        const unsigned int tag = source.codecpar->codec_tag;
        const AVCodecTag* const* tags = context_->oformat->codec_tag;
        stream->codecpar->codec_tag =
            tags != nullptr && av_codec_get_id(tags, tag) == source.codecpar->codec_id ? tag : 0;
        stream->time_base = source.time_base;
        stream->avg_frame_rate = source.avg_frame_rate;
        stream->r_frame_rate = source.r_frame_rate;
        stream->disposition = source.disposition;
        streams_[kind] = stream;
    }

    void close() {
        if (context_ && context_->pb != nullptr) {
            avio_closep(&context_->pb);
        }
    }

    std::filesystem::path output_;
    std::filesystem::path temporary_;
    std::unique_ptr<AVFormatContext, ContextFreer> context_;
    std::array<AVStream*, 2> streams_{nullptr, nullptr};
    std::array<std::int64_t, 2> last_dts_{kNoTimestamp, kNoTimestamp};
    bool finished_ = false;
};

/// First timestamp of the copied streams of a part, in AV_TIME_BASE.
std::int64_t start_of(const Part& part, const JoinedOutput& output) {
    std::int64_t start = std::numeric_limits<std::int64_t>::max();
    for (std::size_t kind : {kVideo, kAudio}) {
        if (part.streams[kind] < 0 || output.stream(kind) == nullptr) {
            continue;
        }
        const AVStream* stream = part.format->streams[part.streams[kind]];
        const std::int64_t first = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        start = std::min(start, av_rescale_q(first, stream->time_base, AV_TIME_BASE_Q));
    }
    return start == std::numeric_limits<std::int64_t>::max() ? 0 : start;
}

/// Copies a part's packets, moved so that the part starts at `offset` (AV_TIME_BASE); returns
/// the part's duration (AV_TIME_BASE).
std::int64_t copy_part(Part& part, JoinedOutput& output, std::int64_t offset,
                       const std::string& label, ProgressReporter& progress) {
    const std::int64_t start = start_of(part, output);
    std::int64_t end = start;
    const std::int64_t size = avio_size(part.format->pb);
    int reported = 0;
    auto packet = ff::make_packet();
    while (true) {
        const int result = av_read_frame(part.format.get(), packet.get());
        if (result == AVERROR_EOF) {
            break;
        }
        ff::check(result, "reading " + part.path.string());
        const auto kind_of = [&]() -> std::optional<std::size_t> {
            for (std::size_t kind : {kVideo, kAudio}) {
                if (packet->stream_index == part.streams[kind] && output.stream(kind) != nullptr) {
                    return kind;
                }
            }
            return std::nullopt;
        };
        const std::optional<std::size_t> kind = kind_of();
        if (!kind) {
            av_packet_unref(packet.get());
            continue;
        }
        const AVStream* in = part.format->streams[packet->stream_index];
        AVStream* out = output.stream(*kind);
        if (packet->duration <= 0 && *kind == kVideo && in->avg_frame_rate.num > 0) {
            packet->duration = av_rescale_q(1, av_inv_q(in->avg_frame_rate), in->time_base);
        }
        if (packet->pts != AV_NOPTS_VALUE) {
            end = std::max(end, av_rescale_q(packet->pts + packet->duration, in->time_base,
                                             AV_TIME_BASE_Q));
        }
        if (size > 0 && packet->pos > 0) {
            const auto step = static_cast<int>(packet->pos * kProgressSteps / size);
            if (step > reported) {
                reported = step;
                progress.report(std::format("{}: {} %", label, step * 100 / kProgressSteps));
            }
        }
        av_packet_rescale_ts(packet.get(), in->time_base, out->time_base);
        const std::int64_t shift = av_rescale_q(offset - start, AV_TIME_BASE_Q, out->time_base);
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts += shift;
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            packet->dts += shift;
        }
        output.write(*packet, *kind);
        av_packet_unref(packet.get());
    }
    return end - start;
}

} // namespace

double FfmpegVideoJoiner::join(std::span<const std::filesystem::path> parts,
                               const std::filesystem::path& output, ProgressReporter& progress) {
    if (std::filesystem::exists(output)) {
        throw MediaError(std::format("{} exists already; merging never replaces a file",
                                     output.string()));
    }
    std::vector<Part> opened;
    for (const auto& path : parts) {
        opened.push_back(open_part(path));
    }
    check_compatible(opened);
    const Part& first = opened.front();
    const auto copied = static_cast<unsigned>(first.streams[kAudio] >= 0 ? 2 : 1);
    if (first.format->nb_streams > copied) {
        progress.report(std::format("Left out: {} other stream(s) of the parts (e.g. camera "
                                    "metadata or timecode)",
                                    first.format->nb_streams - copied));
    }
    JoinedOutput joined(output, first);
    std::int64_t offset = 0;
    for (std::size_t i = 0; i < opened.size(); ++i) {
        const std::string label = std::format("Part {}/{} {}", i + 1, opened.size(),
                                              opened[i].path.filename().string());
        progress.report(label);
        const std::int64_t duration = copy_part(opened[i], joined, offset, label, progress);
        progress.report(std::format("{}: {} long", label,
                                    io::format_clock(static_cast<double>(duration) /
                                                     AV_TIME_BASE)));
        offset += duration;
    }
    joined.finish();
    return static_cast<double>(offset) / AV_TIME_BASE;
}

} // namespace ttrally::cutting
