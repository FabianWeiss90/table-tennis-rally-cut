// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Probing encoders for bit rates that work: the candidates, the file of failures and a probe of
// a software encoder (no GPU needed).

#include "cutting/infrastructure/encoder_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <random>

using namespace ttrally;
using namespace ttrally::cutting;

namespace {

std::filesystem::path temp_dir() {
    std::random_device random;
    auto dir = std::filesystem::temp_directory_path() /
               ("ttrally_probe_" + std::to_string(random()));
    std::filesystem::create_directories(dir);
    return dir;
}

VideoFormat small_format() {
    return {.width = 320,
            .height = 240,
            .sample_aspect_ratio = {1, 1},
            .frame_rate = {30, 1},
            .full_range = false,
            .colorspace = AVCOL_SPC_BT709,
            .color_primaries = AVCOL_PRI_BT709,
            .color_trc = AVCOL_TRC_BT709};
}

} // namespace

TEST_CASE("bit rates are tried from the wanted one outwards, higher first") {
    const auto candidates = bitrate_candidates(6'000'000, 15'000'000);
    REQUIRE(candidates.size() >= 5);
    CHECK(candidates[0] == 6'000'000);
    CHECK(candidates[1] == 6'500'000);
    CHECK(candidates[2] == 5'500'000);
    CHECK(candidates[3] == 7'000'000);
    CHECK(candidates[4] == 5'000'000);
    for (const std::int64_t bitrate : bitrate_candidates(1'500'000, 2'000'000)) {
        CHECK(bitrate >= 1'000'000);
        CHECK(bitrate <= 2'000'000);
    }
    CHECK(bitrate_candidates(20'000'000, 15'000'000).front() == 15'000'000);
}

TEST_CASE("failures are remembered between runs") {
    const auto root = temp_dir();
    const auto file = root / "cache" / "encoder-failures.csv";
    const std::string key = EncoderFailures::key("h264_vaapi", small_format(), 6'000'000);
    {
        EncoderFailures failures(file);
        CHECK_FALSE(failures.known(key));
        failures.add(key, ProbeOutcome::Hangs);
    }
    EncoderFailures again(file);
    CHECK(again.known(key) == ProbeOutcome::Hangs);
    CHECK_FALSE(again.known(EncoderFailures::key("h264_vaapi", small_format(), 8'000'000)));
    again.add(key, ProbeOutcome::Broken);
    CHECK(again.known(key) == ProbeOutcome::Broken);
    std::filesystem::remove_all(root);
}

TEST_CASE("h264_vaapi always runs at constant bit rate, and failures are kept per setting") {
    CHECK(encoder_label("h264_vaapi") == "h264_vaapi rc_mode=CBR aud=1");
    CHECK(encoder_label("hevc_vaapi") == "hevc_vaapi");
    // Failures found with the old default settings do not count for the new ones
    CHECK(EncoderFailures::key("h264_vaapi", small_format(), 4'000'000).starts_with(
        "h264_vaapi rc_mode=CBR aud=1 "));
}

TEST_CASE("a working encoder passes the probe") {
    const VideoFormat format = small_format();
    CHECK(probe_encoder("mpeg4", format, 2'000'000) == ProbeOutcome::Works);
    CHECK(probe_encoder("no_such_encoder", format, 2'000'000) == ProbeOutcome::Unavailable);
}
