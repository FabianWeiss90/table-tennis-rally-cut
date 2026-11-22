// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "cutting/infrastructure/rally_colors.hpp"

#include "cutting/infrastructure/csv_white_balance_store.hpp"
#include "media/application/media_error.hpp"
#include "media/infrastructure/ffmpeg_api.hpp"
#include "media/infrastructure/part_list.hpp"

#include <cmath>
#include <vector>

namespace ttrally::cutting {

namespace fs = std::filesystem;
namespace ff = media::ff;

namespace {

constexpr int kSampleWidth = 192; ///< of the measured pictures (16:9: 192 x 108)
constexpr int kReferenceFrames = 9;
constexpr int kRallyFrames = 7;
constexpr double kTimeEpsilon = 1e-3;

/// The frame shown at `t` (decoded from the keyframe before it), as a small RGB picture.
std::optional<RgbImage> picture_at(AVFormatContext* format, int index, AVCodecContext* decoder,
                                   double t) {
    const AVStream& stream = *format->streams[index];
    const auto target = static_cast<std::int64_t>(std::floor(t / av_q2d(stream.time_base)));
    if (ff::seek_before(format, stream, target) < 0) {
        return std::nullopt;
    }
    avcodec_flush_buffers(decoder);
    auto packet = ff::make_packet();
    auto frame = ff::make_frame();
    while (av_read_frame(format, packet.get()) >= 0) {
        const bool ours = packet->stream_index == index;
        if (ours) {
            avcodec_send_packet(decoder, packet.get());
        }
        av_packet_unref(packet.get());
        while (ours && avcodec_receive_frame(decoder, frame.get()) >= 0) {
            const double at = ff::to_seconds(frame->best_effort_timestamp, stream.time_base);
            if (std::isnan(at) || at < t - kTimeEpsilon) {
                av_frame_unref(frame.get());
                continue;
            }
            const int height = std::max(2, kSampleWidth * frame->height / frame->width / 2 * 2);
            ff::SwsPtr scaler(sws_getContext(frame->width, frame->height,
                                             static_cast<AVPixelFormat>(frame->format),
                                             kSampleWidth, height, AV_PIX_FMT_RGB24, SWS_AREA,
                                             nullptr, nullptr, nullptr));
            if (!scaler) {
                return std::nullopt;
            }
            const bool full = frame->color_range == AVCOL_RANGE_JPEG ||
                              frame->format == AV_PIX_FMT_YUVJ420P;
            const int* coefficients = sws_getCoefficients(frame->colorspace);
            sws_setColorspaceDetails(scaler.get(), coefficients, full ? 1 : 0, coefficients, 1,
                                     0, 1 << 16, 1 << 16);
            RgbImage picture{kSampleWidth, height,
                             std::vector<std::uint8_t>(static_cast<std::size_t>(kSampleWidth) *
                                                       static_cast<std::size_t>(height) * 3)};
            std::uint8_t* planes[1] = {picture.rgb.data()};
            const int strides[1] = {kSampleWidth * 3};
            sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, planes,
                      strides);
            return picture;
        }
    }
    return std::nullopt;
}

} // namespace

RgbImage sampled_picture(const fs::path& video, double start_s, double end_s, int count) {
    try {
        const ff::FormatPtr format = ff::open_input(video);
        const int index =
            av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) {
            return {};
        }
        for (unsigned i = 0; i < format->nb_streams; ++i) {
            if (static_cast<int>(i) != index) {
                format->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        const ff::CodecPtr decoder = ff::open_decoder(format->streams[index]);
        std::vector<RgbImage> pictures;
        for (int k = 0; k < count; ++k) {
            const double t = start_s + (end_s - start_s) * (k + 0.5) / count;
            if (auto picture = picture_at(format.get(), index, decoder.get(), t)) {
                pictures.push_back(std::move(*picture));
            }
        }
        return median_of(pictures);
    } catch (const std::exception&) {
        return {};
    }
}

RallyColors::RallyColors(const fs::path& white_balance) {
    if (white_balance.empty() || !fs::exists(white_balance)) {
        return;
    }
    const fs::path work = white_balance.parent_path();
    settings_ = CsvWhiteBalanceStore().load(work);
    const auto areas = effective_areas(settings_.files);
    for (std::size_t i = 0; i < settings_.files.size(); ++i) {
        areas_[settings_.files[i].file] = areas[i];
    }
    if (!settings_.has_reference()) {
        return;
    }
    const fs::path reference = work.parent_path() / settings_.reference_video;
    const RgbImage picture = sampled_picture(reference, settings_.reference_start_s,
                                             settings_.reference_end_s, kReferenceFrames);
    if (picture.empty()) {
        throw media::MediaError("cannot read the reference rally of the white balance in " +
                                reference.string());
    }
    const double middle = (settings_.reference_start_s + settings_.reference_end_s) / 2;
    target_ = tuned_target(picture, area_at(reference, middle), settings_);
}

ToneCurves RallyColors::for_span(const fs::path& video, double start_s, double end_s) {
    if (!target_) {
        return ToneCurves::identity();
    }
    const RgbImage picture = sampled_picture(video, start_s, end_s, kRallyFrames);
    return rally_correction(picture, area_at(video, (start_s + end_s) / 2), *target_, settings_);
}

std::optional<RelativeRect> RallyColors::area_at(const fs::path& video, double t) const {
    for (const media::PartSpan& part : media::part_spans(video)) {
        if (t >= part.start_s && t < part.end_s) {
            const auto found = areas_.find(part.file.filename().string());
            return found == areas_.end() ? std::nullopt : found->second;
        }
    }
    return std::nullopt;
}

} // namespace ttrally::cutting
