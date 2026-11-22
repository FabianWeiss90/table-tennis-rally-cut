// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_media_probe.hpp"

#include "media/infrastructure/ffmpeg_api.hpp"
#include "media/infrastructure/part_list.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace ttrally::media {

namespace {

Rational to_rational(AVRational value) noexcept { return {value.num, value.den}; }

double start_time_or_zero(const AVStream* stream) noexcept {
    const double start = ff::to_seconds(stream->start_time, stream->time_base);
    return std::isnan(start) ? 0.0 : start;
}

VideoStreamInfo describe_video(const AVStream* stream, int index) {
    VideoStreamInfo video;
    video.index = index;
    video.codec = avcodec_get_name(stream->codecpar->codec_id);
    video.width = stream->codecpar->width;
    video.height = stream->codecpar->height;
    video.time_base = to_rational(stream->time_base);
    video.avg_frame_rate = to_rational(stream->avg_frame_rate);
    video.r_frame_rate = to_rational(stream->r_frame_rate);
    video.start_time_s = start_time_or_zero(stream);
    video.duration_s = ff::to_seconds(stream->duration, stream->time_base);
    return video;
}

AudioStreamInfo describe_audio(const AVStream* stream, int index) {
    AudioStreamInfo audio;
    audio.index = index;
    audio.codec = avcodec_get_name(stream->codecpar->codec_id);
    audio.sample_rate = stream->codecpar->sample_rate;
    audio.channels = stream->codecpar->ch_layout.nb_channels;
    audio.time_base = to_rational(stream->time_base);
    audio.start_time_s = start_time_or_zero(stream);
    audio.duration_s = ff::to_seconds(stream->duration, stream->time_base);
    return audio;
}

/// "2026-11-21T14:03:22.000000Z" in seconds since 1970; nullopt if it is not such a time.
std::optional<std::int64_t> parse_creation_time(const char* text) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (text == nullptr || std::sscanf(text, "%d-%u-%uT%d:%d:%d", &year, &month, &day, &hour,
                                       &minute, &second) != 6) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month},
                                           std::chrono::day{day}};
    if (!date.ok()) {
        return std::nullopt;
    }
    const auto days = std::chrono::sys_days{date}.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(days).count() + hour * 3600LL +
           minute * 60LL + second;
}

std::optional<std::int64_t> creation_time_of(const AVFormatContext& format) {
    if (const AVDictionaryEntry* tag = av_dict_get(format.metadata, "creation_time", nullptr, 0)) {
        return parse_creation_time(tag->value);
    }
    for (unsigned i = 0; i < format.nb_streams; ++i) {
        if (const AVDictionaryEntry* tag =
                av_dict_get(format.streams[i]->metadata, "creation_time", nullptr, 0)) {
            return parse_creation_time(tag->value);
        }
    }
    return std::nullopt;
}

/// When a file was recorded: its creation time, else when it was last written.
std::int64_t recorded_at(const std::filesystem::path& file) {
    try {
        if (const auto created = creation_time_of(*ff::open_input(file))) {
            return *created;
        }
    } catch (const std::exception&) {
        // fall back to the file's time
    }
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error) {
        return 0;
    }
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(written);
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

} // namespace

MediaInfo FfmpegMediaProbe::probe(const std::filesystem::path& path) {
    auto format = ff::open_input(path);
    MediaInfo info;
    info.path = path;
    info.file_size = std::filesystem::file_size(path);
    info.container = format->iformat != nullptr ? format->iformat->name : "";
    info.duration_s = format->duration == AV_NOPTS_VALUE
                          ? std::numeric_limits<double>::quiet_NaN()
                          : static_cast<double>(format->duration) / AV_TIME_BASE;

    if (path.extension() == ff::kPartListExtension) {
        const auto parts = part_spans(path);
        info.recorded_at_s = parts.empty() ? 0 : recorded_at(parts.front().file);
    } else {
        const auto created = creation_time_of(*format);
        info.recorded_at_s = created ? *created : recorded_at(path);
    }

    const int video = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video >= 0) {
        info.video = describe_video(format->streams[video], video);
    }
    const int audio = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio >= 0) {
        info.audio = describe_audio(format->streams[audio], audio);
    }
    return info;
}

} // namespace ttrally::media
