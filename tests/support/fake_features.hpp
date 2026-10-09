// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// In-memory fakes for the frame decoder and the image model, for testing the use cases that
// compute features (features, detection) without media files or ONNX models.

#pragma once

#include "features/application/image_embedder.hpp"
#include "media/application/frame_decoder.hpp"
#include "media/application/media_error.hpp"
#include "support/fake_media.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace ttrally::test {

/// Frames whose pixels all have the value index % 256. Records the decoded frame indices in a
/// list owned by the caller, since the use case destroys the decoder when it is done.
class PatternDecoder final : public media::FrameDecoder {
  public:
    PatternDecoder(media::FrameSize size, std::int64_t frame_count,
                   std::vector<std::int64_t>& decoded, std::optional<std::int64_t> broken_frame)
        : decoded_(decoded), size_(size), frame_count_(frame_count),
          broken_frame_(broken_frame) {}
    media::DecodeBackend backend() const override { return media::DecodeBackend::Cpu; }
    std::int64_t frame_count() const override { return frame_count_; }
    media::FrameSize frame_size() const override { return size_; }
    void decode(std::int64_t first, std::int64_t last, const FrameConsumer& consume) override {
        for (std::int64_t i = first; i <= last; ++i) {
            if (!consume(frame(i))) {
                return;
            }
        }
    }
    void decode_selected(std::span<const std::int64_t> indices,
                         const FrameConsumer& consume) override {
        for (const std::int64_t i : indices) {
            if (i == broken_frame_) {
                throw media::MediaError("broken frame");
            }
            decoded_.push_back(i);
            if (!consume(frame(i))) {
                return;
            }
        }
    }
  private:
    media::VideoFrame frame(std::int64_t index) const {
        media::VideoFrame f;
        f.index = index;
        f.size = size_;
        f.layout = media::PixelLayout::Rgb24;
        f.planes.assign(media::VideoFrame::bytes_for(size_, f.layout),
                        static_cast<std::uint8_t>(index % 256));
        return f;
    }
    std::vector<std::int64_t>& decoded_;
    media::FrameSize size_;
    std::int64_t frame_count_;
    std::optional<std::int64_t> broken_frame_;
};

class PatternDecoders final : public media::FrameDecoderFactory {
  public:
    std::unique_ptr<media::FrameDecoder> open(const std::filesystem::path&,
                                              const media::VideoTimestamps& timestamps,
                                              media::DecodeBackend,
                                              const media::FrameOutput& output) override {
        CHECK(output.layout == media::PixelLayout::Rgb24);
        decoded.clear();
        return std::make_unique<PatternDecoder>(media::FrameSize{output.width, output.height},
                                                timestamps.frame_count(), decoded, broken_frame);
    }
    std::optional<std::int64_t> broken_frame; ///< Decoding this frame fails
    std::vector<std::int64_t> decoded; ///< Frames decoded by the most recently opened decoder
};

/// Feature vector of an image: its first input value, repeated.
class FirstValueEmbedder final : public features::ImageEmbedder {
  public:
    FirstValueEmbedder() {
        info_.model_name = "fake";
        info_.model_fingerprint = "f1";
        info_.input = {{4, 2}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
        info_.parts = {"cls", "mean"};
        info_.part_dims = 1;
    }
    const features::EmbedderInfo& info() const override { return info_; }
    std::vector<float> embed(std::span<const float> images, std::size_t batch) override {
        if (runs == failing_run) {
            throw std::runtime_error("model failed");
        }
        ++runs;
        const std::size_t image_size = 3 * 4 * 2;
        std::vector<float> output;
        for (std::size_t i = 0; i < batch; ++i) {
            output.push_back(images[i * image_size]);
            output.push_back(images[i * image_size]);
        }
        return output;
    }
    int runs = 0;
    int failing_run = -1; ///< This model run throws
    features::EmbedderInfo info_;
};

/// Adds a constant-frame-rate video of `frames` frames (pixels: frame index % 256).
inline void add_pattern_clip(FakeMediaLibrary& library, const std::filesystem::path& path,
                             int frames, int fps) {
    FakeFile file;
    file.info.video = media::VideoStreamInfo{};
    file.info.video->index = 0;
    file.info.video->avg_frame_rate = {fps, 1};
    file.timestamps.time_base = {1, fps};
    for (int i = 0; i < frames; ++i) {
        file.timestamps.pts.push_back(i);
    }
    library.add(path, file);
}

} // namespace ttrally::test
