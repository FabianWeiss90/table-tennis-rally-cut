// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/encoder_probe.hpp"

#include "shared/io/csv.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <thread>

namespace ttrally::cutting {

namespace ff = media::ff;
namespace fs = std::filesystem;

namespace {

constexpr int kProbeFrames = 24; ///< long enough for reordered (B) frames and a few references
/// A probe takes well under a second; a driver that has not finished by then hangs.
constexpr std::chrono::seconds kProbeTimeout{8};
/// The test pattern encodes far better than this; broken headers give ~10 dB or no pictures.
constexpr double kMinPsnrDb = 25.0;
constexpr std::int64_t kBitrateStep = 500'000;
constexpr int kNeighbourSteps = 8; ///< up to 4 Mbit/s either way
constexpr std::int64_t kMinBitrate = 1'000'000;
constexpr int kCell = 32;         ///< checkerboard cell of the test pattern (pixels)
constexpr int kShiftX = 6;        ///< pattern movement per frame (pixels)
constexpr int kShiftY = 3;
constexpr int kDark = 50;
constexpr int kBright = 190;
constexpr int kChromaLow = 90;
constexpr int kChromaSpan = 80;
constexpr double kPeak = 255.0;
constexpr double kDecibels = 10.0;

std::string_view outcome_name(ProbeOutcome outcome) {
    switch (outcome) {
    case ProbeOutcome::Works:
        return "works";
    case ProbeOutcome::Broken:
        return "broken";
    case ProbeOutcome::Hangs:
        return "hangs";
    case ProbeOutcome::Unavailable:
        return "unavailable";
    }
    return "broken";
}

std::optional<ProbeOutcome> parse_outcome(std::string_view text) {
    for (const ProbeOutcome outcome : {ProbeOutcome::Works, ProbeOutcome::Broken,
                                       ProbeOutcome::Hangs, ProbeOutcome::Unavailable}) {
        if (outcome_name(outcome) == text) {
            return outcome;
        }
    }
    return std::nullopt;
}

/// Frame `t` of the test pattern: a moving checkerboard with moving colours.
ff::FramePtr test_picture(const VideoFormat& format, int t) {
    auto frame = ff::make_frame();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = format.width;
    frame->height = format.height;
    frame->color_range = format.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    frame->colorspace = format.colorspace;
    ff::check(av_frame_get_buffer(frame.get(), 0), "probe frame");
    for (int y = 0; y < format.height; ++y) {
        std::uint8_t* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < format.width; ++x) {
            const bool odd = (((x + kShiftX * t) / kCell) + ((y + kShiftY * t) / kCell)) % 2 != 0;
            row[x] = static_cast<std::uint8_t>(odd ? kBright : kDark);
        }
    }
    for (int plane = 1; plane <= 2; ++plane) {
        for (int y = 0; y < (format.height + 1) / 2; ++y) {
            std::uint8_t* row =
                frame->data[plane] + static_cast<std::ptrdiff_t>(y) * frame->linesize[plane];
            for (int x = 0; x < (format.width + 1) / 2; ++x) {
                const int phase = plane == 1 ? x + kShiftX * t : y + kShiftY * t;
                row[x] = static_cast<std::uint8_t>(kChromaLow +
                                                   (phase % kChromaSpan + kChromaSpan) %
                                                       kChromaSpan);
            }
        }
    }
    return frame;
}

double luma_psnr(const AVFrame& a, const AVFrame& b) {
    double squared = 0;
    for (int y = 0; y < a.height; ++y) {
        const std::uint8_t* ra = a.data[0] + static_cast<std::ptrdiff_t>(y) * a.linesize[0];
        const std::uint8_t* rb = b.data[0] + static_cast<std::ptrdiff_t>(y) * b.linesize[0];
        for (int x = 0; x < a.width; ++x) {
            const double d = static_cast<double>(ra[x]) - rb[x];
            squared += d * d;
        }
    }
    const double mse = squared / (static_cast<double>(a.width) * a.height);
    return mse <= 0 ? std::numeric_limits<double>::infinity()
                    : kDecibels * std::log10(kPeak * kPeak / mse);
}

void receive_packets(AVCodecContext* encoder, std::vector<ff::PacketPtr>& packets) {
    for (;;) {
        auto packet = ff::make_packet();
        const int result = avcodec_receive_packet(encoder, packet.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        ff::check(result, "probe encoding");
        packets.push_back(std::move(packet));
    }
}

/// Decodes the packets with FFmpeg's own decoder and compares them with the encoded pictures.
ProbeOutcome compare_decoded(const AVCodecContext& encoder,
                             const std::vector<ff::PacketPtr>& packets,
                             const std::vector<ff::FramePtr>& pictures) {
    const ff::QuietLogScope quiet; // broken streams log many errors
    const AVCodec* codec = avcodec_find_decoder(encoder.codec_id);
    if (codec == nullptr) {
        return ProbeOutcome::Works; // nothing to compare with
    }
    ff::CodecPtr decoder(avcodec_alloc_context3(codec));
    if (encoder.extradata_size > 0) {
        decoder->extradata = static_cast<std::uint8_t*>(
            av_mallocz(static_cast<std::size_t>(encoder.extradata_size) +
                       AV_INPUT_BUFFER_PADDING_SIZE));
        std::copy_n(encoder.extradata, encoder.extradata_size, decoder->extradata);
        decoder->extradata_size = encoder.extradata_size;
    }
    // Any damage counts, also in headers and SEI messages (h264_vaapi on Mesa writes broken
    // picture timing SEIs when it misread the headers; the pictures still decode)
    decoder->err_recognition = AV_EF_EXPLODE | AV_EF_CAREFUL | AV_EF_BITSTREAM | AV_EF_BUFFER;
    if (avcodec_open2(decoder.get(), codec, nullptr) < 0) {
        return ProbeOutcome::Broken;
    }
    std::size_t decoded = 0;
    bool broken = false;
    auto frame = ff::make_frame();
    const auto collect = [&] {
        while (avcodec_receive_frame(decoder.get(), frame.get()) >= 0) {
            if (decoded >= pictures.size() || frame->width != pictures[decoded]->width ||
                frame->height != pictures[decoded]->height ||
                luma_psnr(*frame, *pictures[decoded]) < kMinPsnrDb) {
                broken = true;
            }
            ++decoded;
            av_frame_unref(frame.get());
        }
    };
    for (const ff::PacketPtr& packet : packets) {
        if (avcodec_send_packet(decoder.get(), packet.get()) < 0) {
            broken = true;
        }
        collect();
    }
    avcodec_send_packet(decoder.get(), nullptr);
    collect();
    return broken || decoded != pictures.size() ? ProbeOutcome::Broken : ProbeOutcome::Works;
}

ProbeOutcome run_probe(const std::string& name, const VideoFormat& format,
                       std::int64_t bitrate) {
    std::optional<VideoEncoder> encoder = VideoEncoder::open(name, format, bitrate);
    if (!encoder) {
        return ProbeOutcome::Unavailable;
    }
    std::vector<ff::FramePtr> pictures;
    std::vector<ff::PacketPtr> packets;
    for (int t = 0; t < kProbeFrames; ++t) {
        pictures.push_back(test_picture(format, t));
        encoder->send(*pictures.back(), t);
        receive_packets(encoder->context(), packets);
    }
    ff::check(avcodec_send_frame(encoder->context(), nullptr), "probe flush");
    receive_packets(encoder->context(), packets);
    return compare_decoded(*encoder->context(), packets, pictures);
}

} // namespace

ProbeOutcome probe_encoder(const std::string& encoder, const VideoFormat& format,
                           std::int64_t bitrate) {
    auto promise = std::make_shared<std::promise<ProbeOutcome>>();
    std::future<ProbeOutcome> result = promise->get_future();
    // Detached: if the driver hangs, the thread never returns and cannot be stopped
    std::thread([promise, encoder, format, bitrate] {
        try {
            promise->set_value(run_probe(encoder, format, bitrate));
        } catch (const std::exception&) {
            promise->set_value(ProbeOutcome::Broken);
        }
    }).detach();
    if (result.wait_for(kProbeTimeout) != std::future_status::ready) {
        return ProbeOutcome::Hangs;
    }
    return result.get();
}

std::vector<std::int64_t> bitrate_candidates(std::int64_t wanted, std::int64_t max) {
    wanted = std::clamp(wanted, kMinBitrate, std::max(kMinBitrate, max));
    std::vector<std::int64_t> candidates{wanted};
    for (int step = 1; step <= kNeighbourSteps; ++step) {
        for (const std::int64_t candidate : {wanted + step * kBitrateStep,
                                             wanted - step * kBitrateStep}) {
            if (candidate >= kMinBitrate && candidate <= max &&
                std::ranges::find(candidates, candidate) == candidates.end()) {
                candidates.push_back(candidate);
            }
        }
    }
    return candidates;
}

std::optional<ProbeOutcome> EncoderFailures::known(const std::string& key) const {
    std::error_code error;
    if (!fs::exists(file_, error)) {
        return std::nullopt;
    }
    std::optional<ProbeOutcome> found;
    for (const io::CsvRow& row : io::read_csv(file_)) {
        if (row.size() >= 2 && row[0] == key) {
            found = parse_outcome(row[1]);
        }
    }
    return found;
}

void EncoderFailures::add(const std::string& key, ProbeOutcome outcome) {
    std::vector<io::CsvRow> rows;
    std::error_code error;
    if (fs::exists(file_, error)) {
        rows = io::read_csv(file_);
        if (!rows.empty()) {
            rows.erase(rows.begin()); // header
        }
    }
    std::erase_if(rows, [&](const io::CsvRow& row) { return !row.empty() && row[0] == key; });
    rows.push_back({key, std::string(outcome_name(outcome))});
    fs::create_directories(file_.parent_path(), error);
    io::write_csv(file_, {"encoder_format_bitrate", "outcome"}, rows);
}

std::string EncoderFailures::key(const std::string& encoder, const VideoFormat& format,
                                 std::int64_t bitrate) {
    return std::format("{} {}x{} {}/{} fps {} {}", encoder, format.width, format.height,
                       format.frame_rate.num, format.frame_rate.den,
                       format.full_range ? "full" : "limited", bitrate);
}

fs::path default_encoder_failures_file() {
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    const fs::path directory = base != nullptr ? fs::path(base) : fs::temp_directory_path();
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const fs::path directory = xdg != nullptr && *xdg != '\0' ? fs::path(xdg)
                               : home != nullptr          ? fs::path(home) / ".cache"
                                                          : fs::temp_directory_path();
#endif
    return directory / "ttrally" / "encoder-failures.csv";
}

std::optional<std::int64_t> working_bitrate(const std::string& encoder,
                                            const VideoFormat& format, std::int64_t wanted,
                                            std::int64_t max, EncoderFailures& failures,
                                            ProgressReporter& progress) {
    const auto mbps = [](std::int64_t bitrate) { return static_cast<double>(bitrate) / 1e6; };
    for (const std::int64_t bitrate : bitrate_candidates(wanted, max)) {
        const std::string key = EncoderFailures::key(encoder, format, bitrate);
        std::optional<ProbeOutcome> outcome = failures.known(key);
        const bool remembered = outcome.has_value();
        if (!outcome) {
            outcome = probe_encoder(encoder, format, bitrate);
        }
        if (*outcome == ProbeOutcome::Works) {
            return bitrate;
        }
        if (*outcome == ProbeOutcome::Unavailable) {
            return std::nullopt;
        }
        if (!remembered) {
            failures.add(key, *outcome);
        }
        progress.report(std::format(
            "  {} at {:.1f} Mbit/s {} (a driver bug){}", encoder, mbps(bitrate),
            *outcome == ProbeOutcome::Hangs ? "hangs" : "writes broken pictures",
            remembered ? ", as found before" : ""));
    }
    return std::nullopt;
}

} // namespace ttrally::cutting
