// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "highlights/infrastructure/ffmpeg_playback_source.hpp"

#include "media/application/media_error.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"

namespace ttrally::highlights {

std::unique_ptr<media::FrameDecoder>
FfmpegPlaybackSource::open(const std::filesystem::path& video,
                           const std::filesystem::path& cache_dir, int height) {
    media::FfmpegMediaProbe probe;
    const media::MediaInfo info = probe.probe(video);
    if (!info.video) {
        throw media::MediaError(video.string() + " has no video stream");
    }
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, cache_dir);
    const media::VideoTimestamps timestamps =
        *reader
             .read(video,
                   {.audio_sample_rate = std::nullopt, .video_stream_index = info.video->index})
             .video_timestamps;
    media::FfmpegFrameDecoderFactory decoders;
    return decoders.open(video, timestamps, media::DecodeBackend::Auto, {.height = height});
}

} // namespace ttrally::highlights
