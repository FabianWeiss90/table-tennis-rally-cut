// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The ExtractFeatures use case with in-memory fakes for video, model and storage.

#include "features/application/extract_features.hpp"
#include "features/infrastructure/npy_feature_store.hpp"
#include "shared/io/npy.hpp"
#include "support/fake_media.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>

using namespace ttrally;
using namespace ttrally::features;

namespace {

constexpr int kFps = 60;
constexpr int kFrames = 120; // 2 s

/// Frames whose pixels all have the value index % 256. Records the decoded frame indices in a
/// list owned by the caller, since the use case destroys the decoder when it is done.
class PatternDecoder final : public media::FrameDecoder {
  public:
    PatternDecoder(media::FrameSize size, std::vector<std::int64_t>& decoded)
        : decoded_(decoded), size_(size) {}
    media::DecodeBackend backend() const override { return media::DecodeBackend::Cpu; }
    std::int64_t frame_count() const override { return kFrames; }
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
};

class PatternDecoders final : public media::FrameDecoderFactory {
  public:
    std::unique_ptr<media::FrameDecoder> open(const std::filesystem::path&,
                                              const media::VideoTimestamps&,
                                              media::DecodeBackend,
                                              const media::FrameOutput& output) override {
        CHECK(output.layout == media::PixelLayout::Rgb24);
        decoded.clear();
        return std::make_unique<PatternDecoder>(media::FrameSize{output.width, output.height},
                                                decoded);
    }
    std::vector<std::int64_t> decoded; ///< Frames decoded by the most recently opened decoder
};

/// Feature vector of an image: its first input value, repeated.
class FirstValueEmbedder final : public ImageEmbedder {
  public:
    FirstValueEmbedder() {
        info_.model_name = "fake";
        info_.model_fingerprint = "f1";
        info_.input = {{4, 2}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
        info_.parts = {"cls", "mean"};
        info_.part_dims = 1;
    }
    const EmbedderInfo& info() const override { return info_; }
    std::vector<float> embed(std::span<const float> images, std::size_t batch) override {
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
    EmbedderInfo info_;
};

std::filesystem::path temp_dir() {
    std::random_device device;
    return std::filesystem::temp_directory_path() /
           ("ttrally_features_test_" + std::to_string(device()));
}

struct Fixture {
    test::FakeMediaLibrary library;
    PatternDecoders decoders;
    FirstValueEmbedder embedder;
    std::filesystem::path dir = temp_dir();
    NpyFeatureStore store{dir};
    SilentProgressReporter progress;

    Fixture() {
        test::FakeFile file;
        file.info.video = media::VideoStreamInfo{};
        file.info.video->index = 0;
        file.info.video->avg_frame_rate = {kFps, 1};
        file.timestamps.time_base = {1, kFps};
        for (int i = 0; i < kFrames; ++i) {
            file.timestamps.pts.push_back(i);
        }
        library.add("clip.mp4", file);
    }
    ~Fixture() { std::filesystem::remove_all(dir); }

    ExtractFeaturesResult run(bool force = false) {
        ExtractFeatures extract(library, library, decoders, embedder, store, progress);
        return extract.execute({.video = "clip.mp4",
                                .video_id = "clip",
                                .video_fingerprint = "v1",
                                .sample_rate_hz = 10.0,
                                .batch_size = 4,
                                .decode_backend = media::DecodeBackend::Cpu,
                                .force = force});
    }
};

} // namespace

TEST_CASE("features are computed for every tenth of a second and stored") {
    Fixture fixture;
    const auto result = fixture.run();
    CHECK_FALSE(result.skipped);
    REQUIRE(result.rows == 20); // 0.0 .. 1.9 s
    CHECK(result.dims == 2);
    REQUIRE(fixture.decoders.decoded.size() == 20);
    CHECK(fixture.decoders.decoded[0] == 0);
    CHECK(fixture.decoders.decoded[1] == 6);
    CHECK(fixture.embedder.runs == 5); // 20 frames in batches of 4

    const auto dir = fixture.dir / "clip";
    const auto values = io::read_npy<float>(dir / "features.npy");
    CHECK(values.shape == std::vector<std::size_t>{20, 2});
    // Row k uses frame 6k, whose pixels are 6k / 255
    CHECK(values.data[2 * 3] == Catch::Approx(18.0F / 255.0F));
    const auto frames = io::read_npy<std::int64_t>(dir / "frames.npy");
    CHECK(frames.data[19] == 114);
    const auto times = io::read_npy<double>(dir / "times.npy");
    CHECK(times.data[10] == Catch::Approx(1.0));
    CHECK(std::filesystem::exists(dir / "manifest.json"));
}

TEST_CASE("current features are not recomputed unless forced") {
    Fixture fixture;
    fixture.run();
    const int runs = fixture.embedder.runs;
    CHECK(fixture.run().skipped);
    CHECK(fixture.embedder.runs == runs);

    fixture.embedder.info_.model_fingerprint = "f2"; // another model file
    CHECK_FALSE(fixture.run().skipped);
    CHECK_FALSE(fixture.run(true).skipped);
}
