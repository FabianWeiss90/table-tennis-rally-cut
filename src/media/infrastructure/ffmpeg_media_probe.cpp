// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/ffmpeg_media_probe.hpp"

#include "media/infrastructure/ffmpeg_api.hpp"

#include <cmath>
#include <limits>

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
