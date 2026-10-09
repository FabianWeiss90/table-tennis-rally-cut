// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Decoding, evaluation and the row/frame grid of the detection context. The fixture cases are
// written by training/ttrally_training/make_detection_fixtures.py, so the C++ port is checked
// against the Python code the decoding parameters were tuned with.

#include "detection/domain/decoding.hpp"
#include "detection/domain/evaluation.hpp"
#include "detection/domain/frame_grid.hpp"
#include "shared/io/csv.hpp"
#include "shared/io/json.hpp"
#include "shared/io/npy.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <map>
#include <string>
#include <vector>

using namespace ttrally;
using namespace ttrally::detection;

namespace {

constexpr double kRate = 10.0;

std::vector<float> plateau(std::size_t rows, std::initializer_list<std::pair<int, int>> rallies,
                           float high = 0.9F) {
    std::vector<float> probabilities(rows, 0.0F);
    for (const auto& [first, last] : rallies) {
        for (int row = first; row <= last; ++row) {
            probabilities[static_cast<std::size_t>(row)] = high;
        }
    }
    return probabilities;
}

DecodingParams params_from(const io::JsonValue& json) {
    if (json.at("method").as_string() == "threshold") {
        return ThresholdDecoding{json.at("threshold").as_number(),
                                 json.at("merge_gap_s").as_number(),
                                 json.at("min_rally_s").as_number()};
    }
    return ViterbiDecoding{json.at("mean_rally_s").as_number(), json.at("mean_pause_s").as_number()};
}

} // namespace

TEST_CASE("the threshold merges short interruptions and drops short segments") {
    auto probabilities = plateau(100, {{10, 29}, {33, 49}, {70, 71}});
    const auto segments = decode(probabilities, kRate, ThresholdDecoding{0.5, 0.5, 0.5});
    CHECK(segments == std::vector<RowSegment>{{10, 49}}); // 0.3 s gap merged, 0.2 s blip dropped
}

TEST_CASE("Viterbi ignores an isolated confident row in a pause") {
    auto probabilities = plateau(600, {{100, 199}, {400, 479}}, 0.85F);
    for (auto& p : probabilities) {
        p = std::max(p, 0.15F);
    }
    probabilities[300] = 0.99F;
    const auto segments = decode(probabilities, kRate, ViterbiDecoding{8.0, 20.0});
    CHECK(segments == std::vector<RowSegment>{{100, 199}, {400, 479}});
    CHECK(decode({}, kRate, ViterbiDecoding{}).empty());
}

TEST_CASE("metrics match segments and measure boundaries in seconds") {
    const std::vector<RowSegment> annotated{{10, 59}, {100, 149}, {300, 309}};
    const std::vector<RowSegment> detected{{12, 59}, {100, 155}, {200, 220}};
    const auto metrics = evaluate(detected, annotated, 400, kRate);
    CHECK(metrics.matched == 2);
    CHECK(metrics.precision == Catch::Approx(2.0 / 3));
    CHECK(metrics.start_mae_s == Catch::Approx(0.1));
    CHECK(metrics.end_mae_s == Catch::Approx(0.3));
    const auto perfect = evaluate(annotated, annotated, 400, kRate);
    CHECK(perfect.f1 == 1.0);
    CHECK(perfect.boundary_mae_s == 0.0);
}

TEST_CASE("detections inside ignored sections do not count") {
    std::vector<bool> ignored(200, false);
    std::fill(ignored.begin(), ignored.begin() + 50, true);
    const auto metrics = evaluate({{0, 40}, {100, 149}}, {{100, 149}}, 200, kRate, ignored);
    CHECK(metrics.predicted == 1);
    CHECK(metrics.precision == 1.0);
    CHECK(metrics.row_precision == 1.0);
}

TEST_CASE("rows and frames are translated like in training") {
    // 6 frames per row, video of 60 frames
    const FrameGrid grid({0, 6, 12, 18, 24, 30, 36, 42, 48, 54}, 60);
    CHECK(grid.frames_of({2, 4}).start_frame == 12);
    CHECK(grid.frames_of({2, 4}).end_frame == 29); // up to the frame before row 5
    CHECK(grid.frames_of({8, 9}).end_frame == 59); // the last row runs to the end of the video
    const std::array spans{FrameSpan{12, 30}};
    const auto inside = grid.rows_inside(spans);
    CHECK(inside == std::vector<bool>{false, false, true, true, true, true, false, false, false,
                                      false});
}

TEST_CASE("decoding and metrics give exactly the results of the training code") {
    const std::filesystem::path fixtures = TTRALLY_TEST_FIXTURES;
    const auto probabilities = io::read_npy<float>(fixtures / "decoding_probabilities.npy").data;
    const std::vector<RowSegment> annotated{{40, 99}, {150, 189}, {260, 330}};
    const auto rows = io::read_csv(fixtures / "decoding_expected.csv");
    REQUIRE(rows.size() > 1);

    struct Case {
        std::string params;
        std::vector<RowSegment> segments;
        double f1 = 0.0;
        double boundary_mae_s = 0.0;
        double row_f1 = 0.0;
    };
    std::map<std::string, Case> cases;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        Case& c = cases[row[0]];
        c.params = row[1];
        if (std::stoll(row[2]) >= 0) {
            c.segments.push_back({std::stoll(row[2]), std::stoll(row[3])});
        }
        c.f1 = std::stod(row[4]);
        c.boundary_mae_s = std::stod(row[5]);
        c.row_f1 = std::stod(row[6]);
    }
    REQUIRE(cases.size() == 5);
    for (const auto& [name, expected] : cases) {
        INFO("case " << name << ": " << expected.params);
        const auto segments =
            decode(probabilities, kRate, params_from(io::parse_json(expected.params)));
        CHECK(segments == expected.segments);
        const auto metrics = evaluate(segments, annotated, probabilities.size(), kRate);
        CHECK(metrics.f1 == Catch::Approx(expected.f1).margin(1e-12));
        CHECK(metrics.boundary_mae_s == Catch::Approx(expected.boundary_mae_s).margin(1e-12));
        CHECK(metrics.row_f1 == Catch::Approx(expected.row_f1).margin(1e-12));
    }
}
