// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "features/domain/execution_provider.hpp"
#include "features/domain/model_input.hpp"
#include "features/domain/sampling.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ttrally::features;
using ttrally::FrameTimeline;

TEST_CASE("samples follow a time grid independent of the frame rate") {
    // 59.94 fps, 10 s: frames 0..599
    const auto timeline = FrameTimeline::constant({60000, 1001}, 0.5);
    const auto samples = plan_samples(timeline, 600, 10.0);
    REQUIRE(samples.size() == 100); // 0.0 .. 9.9 s after the first frame
    CHECK(samples[0].time_s == Catch::Approx(0.5));
    CHECK(samples[0].frame == 0);
    CHECK(samples[1].frame == 6);  // 0.1 s * 59.94 = 5.99
    CHECK(samples[50].frame == 300); // 5 s = 299.7
    CHECK(samples[99].time_s == Catch::Approx(0.5 + 9.9));
}

TEST_CASE("variable frame rate can map two samples to the same frame") {
    // A gap of 0.3 s between frames 1 and 2
    const auto timeline = FrameTimeline::variable({0.0, 0.1, 0.4, 0.5}, {10, 1});
    const auto samples = plan_samples(timeline, 4, 10.0);
    REQUIRE(samples.size() == 6);
    CHECK(samples[2].frame == 1); // 0.2 s: nearest is 0.1
    CHECK(samples[3].frame == 2); // 0.3 s: nearest is 0.4
    const auto frames = frames_to_decode(samples);
    CHECK(frames == std::vector<std::int64_t>{0, 1, 2, 3});
}

TEST_CASE("RGB frames become normalised planar model input") {
    ModelInputSpec spec;
    spec.size = {2, 1};
    spec.mean = {0.5F, 0.5F, 0.5F};
    spec.stddev = {0.5F, 0.25F, 1.0F};
    ttrally::media::VideoFrame frame;
    frame.size = {2, 1};
    frame.layout = ttrally::media::PixelLayout::Rgb24;
    frame.planes = {255, 0, 51, 0, 255, 102}; // two pixels: (255,0,51) and (0,255,102)

    std::vector<float> batch;
    append_model_input(frame, spec, batch);
    REQUIRE(batch.size() == 6);
    CHECK(batch[0] == Catch::Approx(1.0F));   // R of pixel 0: (1 - 0.5) / 0.5
    CHECK(batch[1] == Catch::Approx(-1.0F));  // R of pixel 1
    CHECK(batch[2] == Catch::Approx(-2.0F));  // G of pixel 0: (0 - 0.5) / 0.25
    CHECK(batch[3] == Catch::Approx(2.0F));   // G of pixel 1
    CHECK(batch[4] == Catch::Approx(-0.3F));  // B of pixel 0: 0.2 - 0.5
    CHECK(batch[5] == Catch::Approx(-0.1F));  // B of pixel 1: 0.4 - 0.5

    frame.size = {1, 2};
    CHECK_THROWS_AS(append_model_input(frame, spec, batch), std::invalid_argument);
}

TEST_CASE("execution provider names round trip") {
    for (const auto& name : execution_provider_names()) {
        REQUIRE(parse_execution_provider(name));
        CHECK(to_string(*parse_execution_provider(name)) == name);
    }
    CHECK(automatic_provider_order().back() == ExecutionProvider::Cpu);
}
