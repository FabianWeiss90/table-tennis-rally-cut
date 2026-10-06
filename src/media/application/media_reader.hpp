// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include "media/domain/audio_signal.hpp"
#include "media/domain/video_timestamps.hpp"

#include <filesystem>
#include <optional>

namespace ttrally::media {

/// What to extract from a media file in one pass.
struct ReadRequest {
    std::optional<int> audio_sample_rate;  ///< Decode the best audio stream to mono at this rate
    std::optional<int> video_stream_index; ///< Collect the frame timestamps of this video stream
};

/// Result of a read; each member is set if it was requested.
struct MediaContent {
    std::optional<AudioSignal> audio;
    std::optional<VideoTimestamps> video_timestamps;
};

/// Port: reads audio and frame timestamps from a media file in a single pass over the file.
/// No loudness normalisation is applied. Throws MediaError on failure.
class MediaReader {
  public:
    virtual ~MediaReader() = default;
    [[nodiscard]] virtual MediaContent read(const std::filesystem::path& path,
                                            const ReadRequest& request) = 0;
};

} // namespace ttrally::media
