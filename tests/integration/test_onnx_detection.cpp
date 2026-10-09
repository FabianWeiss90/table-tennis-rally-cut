// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// ONNX Runtime adapter of the rally detector with a tiny untrained detector of the same
// interface as the exported one (tests/fixtures/tiny_detector.onnx and the probabilities PyTorch
// computes for a fixed input, made by training/ttrally_training/make_detection_fixtures.py), and
// `detect` end-to-end on a clip generated with the ffmpeg command-line tool.

#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "detection/application/detect_rallies.hpp"
#include "detection/infrastructure/csv_detection_output.hpp"
#include "detection/infrastructure/onnx_rally_detector.hpp"
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
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <random>

using namespace ttrally;
using namespace ttrally::detection;

namespace {

const std::filesystem::path kFixtures = TTRALLY_TEST_FIXTURES;

#ifdef _WIN32
constexpr const char* kNullRedirect = " >NUL 2>&1";
#else
constexpr const char* kNullRedirect = " >/dev/null 2>&1";
#endif

bool run(const std::string& command) {
    return std::system((command + kNullRedirect).c_str()) == 0;
}

} // namespace

TEST_CASE("the ONNX detector reads its metadata and computes what PyTorch computes") {
    OnnxRallyDetector detector(kFixtures / "tiny_detector.onnx");
    const DetectorInfo& info = detector.info();
    CHECK(info.backbone == "tiny-test-model");
    CHECK(info.backbone_execution_provider == "cpu");
    CHECK(info.parts.size() == 6);
    CHECK(info.dims() == 12);
    CHECK(info.input_width == 42);
    CHECK(info.input_height == 28);
    CHECK(info.sample_rate_hz == 10.0);
    REQUIRE(std::holds_alternative<ThresholdDecoding>(info.decoding));
    CHECK(std::get<ThresholdDecoding>(info.decoding).merge_gap_s == 1.0);

    const auto input = io::read_npy<float>(kFixtures / "tiny_detector_input.npy");
    const auto expected = io::read_npy<float>(kFixtures / "tiny_detector_output.npy").data;
    const auto probabilities = detector.probabilities(input.data, input.shape[0]);
    REQUIRE(probabilities.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(probabilities[i] == Catch::Approx(expected[i]).margin(1e-5));
    }
    CHECK_THROWS_AS(detector.probabilities(input.data, input.shape[0] + 1), std::invalid_argument);
}

TEST_CASE("models that are no rally detector are refused") {
    CHECK_THROWS_AS(OnnxRallyDetector(kFixtures / "tiny_backbone.onnx"), std::runtime_error);
    CHECK(std::holds_alternative<ViterbiDecoding>(
        parse_decoding(R"({"method": "viterbi", "mean_rally_s": 6, "mean_pause_s": 20})")));
    CHECK_THROWS_AS(parse_decoding(R"({"method": "magic"})"), std::runtime_error);
}

TEST_CASE("detect end-to-end on a generated clip") {
    if (!run("ffmpeg -hide_banner -version")) {
        SKIP("ffmpeg command-line tool not found");
    }
    media::configure_ffmpeg_logging(false);
    std::random_device device;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("ttrally_detect_test_{:08x}", device());
    std::filesystem::create_directories(dir);
    const auto clip = dir / "clip.mp4";
    REQUIRE(run(std::format("ffmpeg -hide_banner -y -f lavfi -i testsrc2=size=320x180:rate=60 "
                            "-t 4 -c:v mpeg4 -q:v 4 \"{}\"",
                            clip.string())));

    media::FfmpegMediaProbe probe;
    media::FfmpegMediaReader ffmpeg_reader;
    media::CachingMediaReader reader(ffmpeg_reader, dir / "cache");
    media::FfmpegFrameDecoderFactory decoders;
    features::OnnxImageEmbedder backbone(kFixtures / "tiny_backbone.onnx",
                                         features::ExecutionProvider::Cpu);
    features::NpyFeatureStore store(dir / "features");
    SilentProgressReporter progress;
    features::ExtractFeatures extract(probe, reader, decoders, backbone, store, progress);
    OnnxRallyDetector detector(kFixtures / "tiny_detector.onnx");
    CsvDetectionOutput output(dir / "detections");
    DetectRallies detect(extract, backbone, store, detector, nullptr, nullptr, output, progress);

    const auto result = detect.execute({.features = {.video = clip,
                                                     .video_id = "clip",
                                                     .video_fingerprint = "v",
                                                     .batch_size = 8,
                                                     .decode_backend = media::DecodeBackend::Cpu},
                                        .evaluate = false});
    CHECK(result.detection.probabilities.size() == 40);
    CHECK_FALSE(result.evaluation);
    // The detections can be opened like labels
    const auto saved = annotation::CsvAnnotationRepository(dir / "detections").load("clip");
    CHECK(saved.rallies.size() == result.detection.rallies.size());
    CHECK(io::read_npy<float>(dir / "detections" / "clip.probabilities.npy").data.size() == 40);
    std::filesystem::remove_all(dir);
}
