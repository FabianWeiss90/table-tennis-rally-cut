// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The ExtractFeatures use case with in-memory fakes for video, model and storage.

#include "features/application/extract_features.hpp"
#include "features/infrastructure/npy_feature_store.hpp"
#include "media/application/media_error.hpp"
#include "shared/io/npy.hpp"
#include "support/fake_features.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <stdexcept>

using namespace ttrally;
using namespace ttrally::features;

namespace {

constexpr int kFps = 60;
constexpr int kFrames = 120; // 2 s

std::filesystem::path temp_dir() {
    std::random_device device;
    return std::filesystem::temp_directory_path() /
           ("ttrally_features_test_" + std::to_string(device()));
}

struct Fixture {
    test::FakeMediaLibrary library;
    test::PatternDecoders decoders;
    test::FirstValueEmbedder embedder;
    std::filesystem::path dir = temp_dir();
    NpyFeatureStore store{dir};
    SilentProgressReporter progress;

    Fixture() { test::add_pattern_clip(library, "clip.mp4", kFrames, kFps); }
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
    std::ifstream manifest(dir / "manifest.json");
    const std::string text{std::istreambuf_iterator<char>(manifest), {}};
    CHECK(text.find("\"execution_provider\": \"cpu\"") != std::string::npos);
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

TEST_CASE("decoding errors stop the feature extraction") {
    Fixture fixture;
    fixture.decoders.broken_frame = 60;
    CHECK_THROWS_AS(fixture.run(), media::MediaError);
    CHECK_FALSE(std::filesystem::exists(fixture.dir / "clip" / "features.npy"));
}

TEST_CASE("model errors stop the decoding") {
    Fixture fixture;
    fixture.embedder.failing_run = 0;
    CHECK_THROWS_AS(fixture.run(), std::runtime_error);
    // At most the failed batch, two queued ones and the one waiting for room: 4 of 5 batches
    CHECK(fixture.decoders.decoded.size() <= 16);
    CHECK_FALSE(std::filesystem::exists(fixture.dir / "clip" / "features.npy"));
}
