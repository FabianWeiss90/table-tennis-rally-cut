// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "media/infrastructure/caching_media_reader.hpp"

#include "shared/io/file_cache.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <system_error>

namespace ttrally::media {

namespace {

// Timestamp cache: int64 version, int64 time base num/den, int64 count, count x int64 PTS
constexpr std::int64_t kTimestampCacheVersion = 2;
// Audio cache: raw float32 samples in <key>.f32 and a text file <key>.meta
constexpr std::string_view kAudioCacheHeader = "ttrally-audio-cache 1";

std::filesystem::path with_suffix(std::filesystem::path path, std::string_view suffix) {
    path += suffix;
    return path;
}

std::optional<double> parse_double(std::string_view text) {
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

template <typename T> void write_value(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T> bool read_value(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

class TimestampCache {
  public:
    TimestampCache(const std::filesystem::path& dir, const std::filesystem::path& media,
                   int stream_index)
        : file_(dir / (io::cache_key(media, std::format("video-pts-{}", stream_index)) + ".pts")) {}

    [[nodiscard]] std::optional<VideoTimestamps> load() const {
        std::ifstream in(file_, std::ios::binary);
        std::int64_t version = 0;
        std::int64_t count = 0;
        VideoTimestamps timestamps;
        if (!in || !read_value(in, version) || version != kTimestampCacheVersion ||
            !read_value(in, timestamps.time_base.num) ||
            !read_value(in, timestamps.time_base.den) || !read_value(in, count) || count <= 0) {
            return std::nullopt;
        }
        timestamps.pts.resize(static_cast<std::size_t>(count));
        in.read(reinterpret_cast<char*>(timestamps.pts.data()),
                static_cast<std::streamsize>(timestamps.pts.size() * sizeof(std::int64_t)));
        if (!in) {
            return std::nullopt;
        }
        return timestamps;
    }

    void store(const VideoTimestamps& timestamps) const {
        io::write_file_atomically(file_, [&timestamps](std::ofstream& out) {
            write_value(out, kTimestampCacheVersion);
            write_value(out, timestamps.time_base.num);
            write_value(out, timestamps.time_base.den);
            write_value(out, static_cast<std::int64_t>(timestamps.pts.size()));
            out.write(reinterpret_cast<const char*>(timestamps.pts.data()),
                      static_cast<std::streamsize>(timestamps.pts.size() * sizeof(std::int64_t)));
        });
    }

  private:
    std::filesystem::path file_;
};

class AudioCache {
  public:
    AudioCache(const std::filesystem::path& dir, const std::filesystem::path& media,
               int sample_rate)
        : base_(dir / io::cache_key(media, std::format("audio-mono-{}", sample_rate))),
          sample_rate_(sample_rate) {}

    [[nodiscard]] std::optional<AudioSignal> load() const {
        std::ifstream meta(with_suffix(base_, ".meta"));
        std::string header;
        std::string start_key;
        std::string start_text;
        std::string count_key;
        std::size_t count = 0;
        std::getline(meta, header);
        meta >> start_key >> start_text >> count_key >> count;
        const auto start = parse_double(start_text);
        std::error_code error;
        const auto samples_file = with_suffix(base_, ".f32");
        const auto bytes = std::filesystem::file_size(samples_file, error);
        if (!meta || header != kAudioCacheHeader || start_key != "start_time" || !start ||
            count_key != "samples" || error || bytes != count * sizeof(float)) {
            return std::nullopt;
        }
        AudioSignal signal;
        signal.sample_rate = sample_rate_;
        signal.start_time_s = *start;
        signal.samples.resize(count);
        std::ifstream in(samples_file, std::ios::binary);
        in.read(reinterpret_cast<char*>(signal.samples.data()),
                static_cast<std::streamsize>(bytes));
        if (!in) {
            return std::nullopt;
        }
        return signal;
    }

    void store(const AudioSignal& signal) const {
        io::write_file_atomically(with_suffix(base_, ".f32"), [&signal](std::ofstream& out) {
            out.write(reinterpret_cast<const char*>(signal.samples.data()),
                      static_cast<std::streamsize>(signal.samples.size() * sizeof(float)));
        });
        // Written last: its presence marks the entry as complete.
        io::write_file_atomically(with_suffix(base_, ".meta"), [&signal](std::ofstream& out) {
            out << kAudioCacheHeader << '\n'
                << "start_time " << std::format("{:.17g}", signal.start_time_s) << '\n'
                << "samples " << signal.samples.size() << '\n';
        });
    }

  private:
    std::filesystem::path base_;
    int sample_rate_;
};

} // namespace

MediaContent CachingMediaReader::read(const std::filesystem::path& path,
                                      const ReadRequest& request) {
    MediaContent content;
    std::optional<AudioCache> audio_cache;
    std::optional<TimestampCache> timestamp_cache;
    ReadRequest missing;

    if (request.audio_sample_rate) {
        audio_cache.emplace(cache_dir_, path, *request.audio_sample_rate);
        content.audio = audio_cache->load();
        if (!content.audio) {
            missing.audio_sample_rate = request.audio_sample_rate;
        }
    }
    if (request.video_stream_index) {
        timestamp_cache.emplace(cache_dir_, path, *request.video_stream_index);
        content.video_timestamps = timestamp_cache->load();
        if (!content.video_timestamps) {
            missing.video_stream_index = request.video_stream_index;
        }
    }
    if (!missing.audio_sample_rate && !missing.video_stream_index) {
        return content;
    }

    MediaContent fresh = inner_.read(path, missing);
    if (fresh.audio) {
        audio_cache->store(*fresh.audio);
        content.audio = std::move(fresh.audio);
    }
    if (fresh.video_timestamps) {
        timestamp_cache->store(*fresh.video_timestamps);
        content.video_timestamps = std::move(fresh.video_timestamps);
    }
    return content;
}

} // namespace ttrally::media
