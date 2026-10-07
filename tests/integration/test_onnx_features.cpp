// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// ONNX Runtime adapter with a tiny model of the same interface as the exported backbone
// (tests/fixtures/tiny_backbone.onnx, made by training/ttrally_training/make_test_model.py), and
// `features` end-to-end on a clip generated with the ffmpeg command-line tool.

#include "features/application/extract_features.hpp"
#include "features/infrastructure/npy_feature_store.hpp"
#include "features/infrastructure/onnx_image_embedder.hpp"
#include "media/infrastructure/caching_media_reader.hpp"
#include "media/infrastructure/ffmpeg_frame_decoder.hpp"
#include "media/infrastructure/ffmpeg_logging.hpp"
#include "media/infrastructure/ffmpeg_media_probe.hpp"
#include "media/infrastructure/ffmpeg_media_reader.hpp"
#include "shared/io/npy.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>

using namespace ttrally;
using namespace ttrally::features;

namespace {

const std::filesystem::path kTinyModel =
    std::filesystem::path(TTRALLY_TEST_FIXTURES) / "tiny_backbone.onnx";

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

} // namespace

TEST_CASE("the ONNX embedder reads the model metadata and runs on the CPU") {
    OnnxImageEmbedder embedder(kTinyModel, ExecutionProvider::Cpu);
    const auto& info = embedder.info();
    CHECK(info.model_name == "tiny-test-model");
    CHECK(info.input.size.width == 42);
    CHECK(info.input.size.height == 28);
    CHECK(info.input.mean[0] == Catch::Approx(0.485F));
    CHECK(info.parts.size() == 6);
    CHECK(info.dims() == 12);
    CHECK(info.provider == ExecutionProvider::Cpu);

    // Two images: all values 1.0 and all values -2.0; the model returns their means.
    const std::size_t image = 3 * 28 * 42;
    std::vector<float> images(2 * image, 1.0F);
    std::fill(images.begin() + image, images.end(), -2.0F);
    const auto output = embedder.embed(images, 2);
    REQUIRE(output.size() == 24);
    CHECK(output[0] == Catch::Approx(1.0F));
    CHECK(output[11] == Catch::Approx(1.0F));
    CHECK(output[12] == Catch::Approx(-2.0F));
    CHECK_THROWS_AS(embedder.embed(images, 3), std::invalid_argument);
}

TEST_CASE("auto picks a provider compiled into ONNX Runtime") {
    OnnxImageEmbedder embedder(kTinyModel, ExecutionProvider::Auto);
    const auto compiled = compiled_execution_providers();
    CHECK(std::find(compiled.begin(), compiled.end(), embedder.info().provider) != compiled.end());
}

TEST_CASE("features end-to-end on a generated clip") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_onnx_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    const auto clip = dir / "clip.mp4";
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate=60000/1001 "
                            "-t 3 -c:v mpeg4 -q:v 4 \"{}\"",
                            clip.string())));

    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, dir / "cache");
    media::FfmpegFrameDecoderFactory decoders;
    OnnxImageEmbedder embedder(kTinyModel, ExecutionProvider::Cpu);
    NpyFeatureStore store(dir / "features");
    SilentProgressReporter progress;
    ExtractFeatures extract(probe, reader, decoders, embedder, store, progress);
    const auto result = extract.execute({.video = clip,
                                         .video_id = "clip",
                                         .video_fingerprint = "v",
                                         .sample_rate_hz = 10.0,
                                         .batch_size = 8,
                                         .decode_backend = media::DecodeBackend::Cpu,
                                         .force = false});
    CHECK(result.rows == 30);
    const auto values = io::read_npy<float>(dir / "features" / "clip" / "features.npy");
    CHECK(values.shape == std::vector<std::size_t>{30, 12});
    std::filesystem::remove_all(dir);
}
