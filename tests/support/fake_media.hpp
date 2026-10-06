// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// In-memory implementations of the media ports, for testing use cases without media files.

#pragma once

#include "media/application/frame_sampler.hpp"
#include "media/application/media_error.hpp"
#include "media/application/media_probe.hpp"
#include "media/application/media_reader.hpp"

#include <functional>
#include <map>
#include <string>

namespace ttrally::test {

/// A media file as the fakes know it.
struct FakeFile {
    media::MediaInfo info;
    media::AudioSignal audio; ///< Already at the requested sample rate
    media::VideoTimestamps timestamps;
};

class FakeMediaLibrary final : public media::MediaProbe, public media::MediaReader {
  public:
    void add(const std::filesystem::path& path, FakeFile file) {
        file.info.path = path;
        files_[path.string()] = std::move(file);
    }

    media::MediaInfo probe(const std::filesystem::path& path) override {
        return find(path).info;
    }

    media::MediaContent read(const std::filesystem::path& path,
                             const media::ReadRequest& request) override {
        const FakeFile& file = find(path);
        media::MediaContent content;
        if (request.audio_sample_rate) {
            content.audio = file.audio;
        }
        if (request.video_stream_index) {
            content.video_timestamps = file.timestamps;
        }
        ++reads;
        return content;
    }

    int reads = 0;

  private:
    const FakeFile& find(const std::filesystem::path& path) const {
        const auto it = files_.find(path.string());
        if (it == files_.end()) {
            throw media::MediaError("cannot open " + path.string());
        }
        return it->second;
    }

    std::map<std::string, FakeFile> files_;
};

/// Frame sampler that renders an image from the requested time with a given function.
class FakeFrameSamplerFactory final : public media::FrameSamplerFactory {
  public:
    using Renderer = std::function<media::GrayImage(const std::filesystem::path&, double)>;

    explicit FakeFrameSamplerFactory(Renderer renderer) : renderer_(std::move(renderer)) {}

    std::unique_ptr<media::FrameSampler> open(const std::filesystem::path& path,
                                              media::DecodeBackend /*requested*/) override {
        return std::make_unique<Sampler>(path, renderer_);
    }

  private:
    class Sampler final : public media::FrameSampler {
      public:
        Sampler(std::filesystem::path path, Renderer renderer)
            : path_(std::move(path)), renderer_(std::move(renderer)) {}
        media::DecodeBackend backend() const override { return media::DecodeBackend::Cpu; }
        std::optional<media::GrayImage> sample_gray(double t, int /*width*/,
                                                    int /*height*/) override {
            return renderer_(path_, t);
        }

      private:
        std::filesystem::path path_;
        Renderer renderer_;
    };

    Renderer renderer_;
};

} // namespace ttrally::test
