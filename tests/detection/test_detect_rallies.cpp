// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The DetectRallies use case with in-memory fakes for video, image model, detector, labels and
// output.

#include "detection/application/detect_rallies.hpp"
#include "features/infrastructure/npy_feature_store.hpp"
#include "support/fake_features.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>

using namespace ttrally;
using namespace ttrally::detection;

namespace {

constexpr int kFps = 60;
constexpr int kFrames = 240; // 4 s, 40 feature rows (the pixel pattern repeats after 256)

/// Rally probability of a row: 1 if its feature (the frame index / 255) lies in [low, high].
class RangeDetector final : public RallyDetector {
  public:
    RangeDetector() {
        info_.backbone = "fake";
        info_.backbone_execution_provider = "cpu";
        info_.parts = {"cls", "mean"};
        info_.part_dims = 1;
        info_.input_width = 4;
        info_.input_height = 2;
        info_.sample_rate_hz = 10.0;
        info_.decoding = ThresholdDecoding{0.5, 0.0, 0.0};
    }
    const DetectorInfo& info() const override { return info_; }
    std::vector<float> probabilities(std::span<const float> features, std::size_t rows) override {
        std::vector<float> result;
        for (std::size_t row = 0; row < rows; ++row) {
            const float frame = features[row * info_.dims()] * 255.0F;
            result.push_back(frame >= low && frame <= high ? 1.0F : 0.0F);
        }
        return result;
    }
    float low = 60.0F;  // frames 60..119: rows 10..19
    float high = 119.0F;
    DetectorInfo info_;
};

class MemoryLabels final : public annotation::AnnotationRepository {
  public:
    annotation::StoredLabels load(const std::string&) override { return labels; }
    void save(const annotation::AnnotationSheet&) override {}
    annotation::StoredLabels labels;
};

class MemoryReviews final : public annotation::ReviewStateStore {
  public:
    annotation::ReviewStatusMap load(const std::string&) override { return statuses; }
    void save(const std::string&, const std::vector<annotation::ReviewItem>&) override {}
    annotation::ReviewStatusMap statuses;
};

class MemoryOutput final : public DetectionOutput {
  public:
    void save(const Detection& detection) override { saved = detection; }
    std::string location(const std::string& id) const override { return id; }
    std::optional<Detection> saved;
};

std::filesystem::path temp_dir() {
    std::random_device device;
    return std::filesystem::temp_directory_path() /
           ("ttrally_detect_test_" + std::to_string(device()));
}

struct Fixture {
    test::FakeMediaLibrary library;
    test::PatternDecoders decoders;
    test::FirstValueEmbedder embedder;
    std::filesystem::path dir = temp_dir();
    features::NpyFeatureStore store{dir};
    SilentProgressReporter progress;
    RangeDetector detector;
    MemoryLabels labels;
    MemoryReviews reviews;
    MemoryOutput output;

    Fixture() { test::add_pattern_clip(library, "clip.mp4", kFrames, kFps); }
    ~Fixture() { std::filesystem::remove_all(dir); }

    DetectRalliesResult run() {
        features::ExtractFeatures extract(library, library, decoders, embedder, store, progress);
        DetectRallies detect(extract, embedder, store, detector, &labels, &reviews, output,
                             progress);
        return detect.execute({.features = {.video = "clip.mp4",
                                            .video_id = "clip",
                                            .video_fingerprint = "v1",
                                            .sample_rate_hz = 2.0, // overridden by the detector
                                            .batch_size = 8,
                                            .decode_backend = media::DecodeBackend::Cpu}});
    }
};

} // namespace

TEST_CASE("rallies are detected, saved in frames and times, and evaluated") {
    Fixture fixture;
    fixture.labels.labels.rallies = {{60, 119, {}, false, ""}};
    fixture.reviews.statuses = {{{annotation::ReviewKind::Gap, 1}, annotation::ReviewStatus::Reviewed}};
    const auto result = fixture.run();

    REQUIRE(result.detection.rallies.size() == 1);
    const auto& rally = result.detection.rallies[0];
    CHECK(rally.frames.start_frame == 60);
    CHECK(rally.frames.end_frame == 119);
    CHECK(rally.start_s == Catch::Approx(1.0));
    CHECK(rally.end_s == Catch::Approx(2.0));
    CHECK(rally.mean_probability == 1.0);
    CHECK(result.detection.probabilities.size() == 40); // 10 per second, as the detector says
    CHECK(result.warnings.empty());
    REQUIRE(fixture.output.saved);
    CHECK(fixture.output.saved->video_frame_count == kFrames);

    REQUIRE(result.evaluation);
    CHECK(result.evaluation->metrics.f1 == 1.0);
    CHECK(result.evaluation->labels_complete);
}

TEST_CASE("stored features are reused") {
    Fixture fixture;
    static_cast<void>(fixture.run());
    const int runs = fixture.embedder.runs;
    static_cast<void>(fixture.run());
    CHECK(fixture.embedder.runs == runs);
}

TEST_CASE("an image model other than the detector's is refused before any work") {
    Fixture fixture;
    fixture.detector.info_.backbone = "another model";
    CHECK_THROWS_AS(fixture.run(), IncompatibleFeatures);
    CHECK(fixture.embedder.runs == 0);
    CHECK_FALSE(fixture.output.saved);
}

TEST_CASE("features from another execution provider give a warning") {
    Fixture fixture;
    fixture.detector.info_.backbone_execution_provider = "webgpu";
    const auto result = fixture.run();
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("webgpu") != std::string::npos);
}

TEST_CASE("videos without labels are not evaluated; open reviews are reported") {
    Fixture fixture;
    CHECK_FALSE(fixture.run().evaluation);

    fixture.labels.labels.rallies = {{60, 119, {}, false, ""}};
    fixture.labels.labels.ignored = {{0, 29, ""}};
    fixture.reviews.statuses = {{{annotation::ReviewKind::Gap, 1}, annotation::ReviewStatus::Open}};
    const auto evaluation = fixture.run().evaluation;
    REQUIRE(evaluation);
    CHECK_FALSE(evaluation->labels_complete);
    CHECK(evaluation->ignored_rows == 5);
}
