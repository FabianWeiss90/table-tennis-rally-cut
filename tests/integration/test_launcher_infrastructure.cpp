// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The workspace scanner on a small project directory, and the process runner with the cmake
// command-line tool as a portable test program.

#include "launcher/infrastructure/ffmpeg_video_preview.hpp"
#include "launcher/infrastructure/filesystem_workspace_scanner.hpp"
#include "launcher/infrastructure/subprocess_runner.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <format>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace ttrally::launcher;
namespace fs = std::filesystem;

namespace {

fs::path temp_dir() {
    std::random_device device;
    return fs::temp_directory_path() / ("ttrally_launcher_test_" + std::to_string(device()));
}

void touch(const fs::path& path, const std::string& content = "") {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

/// Waits until the runner's program has ended (at most 30 s).
void wait_for(const SubprocessRunner& runner) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (runner.running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST_CASE("the scanner finds videos, their pre-cuts and their progress") {
    const fs::path root = temp_dir();
    touch(root / "data" / "Match-A_FHD.MP4");
    touch(root / "data" / "Match-A_FHD_cut_12_rallies.mp4");
    touch(root / "data" / "Match-B.mov");
    touch(root / "data" / "notes.txt");
    touch(root / "data" / "align" / "match-a.csv");
    touch(root / "data" / "annotations" / "match-a.csv",
          "video_id,rally_id,start_frame,end_frame,fps,serve_contact_frame,flags,notes\n"
          "match-a,1,10,20,60,,,\nmatch-a,,30,40,60,,ignore,\n");
    touch(root / "data" / "annotations" / "match-a.review.csv",
          "kind,id,first_frame,last_frame,status\ngap,1,0,9,reviewed\ncandidate,1,10,20,annotated\n");
    touch(root / "data" / "features" / "match-a" / "manifest.json",
          R"json({"model": "facebook/dinov2-base (fp16)"})json");
    touch(root / "data" / "cuts" / "match-a.mp4");
    touch(root / "data" / "models" / "dinov2-vitb14-fp16.onnx");
    touch(root / "data" / "models" / "rally-detector-test.onnx");
    touch(root / "weights" / "rally-detector.onnx");

    const WorkspaceSnapshot workspace = FilesystemWorkspaceScanner(root).scan();
    REQUIRE(workspace.videos.size() == 2);
    const VideoEntry& a = workspace.videos[0];
    CHECK(a.original == "data/Match-A_FHD.MP4");
    CHECK(a.video_id == "match-a");
    CHECK(a.cut_candidates == std::vector<std::string>{"data/Match-A_FHD_cut_12_rallies.mp4"});
    CHECK(a.aligned);
    CHECK(a.annotation == AnnotationProgress::Complete);
    CHECK(a.rallies == 1); // the ignored section is no rally
    CHECK(a.features_model == "facebook/dinov2-base (fp16)");
    CHECK_FALSE(a.detected);
    CHECK(a.cuts == std::vector<std::string>{"data/cuts/match-a.mp4"});
    CHECK(workspace.videos[1].annotation == AnnotationProgress::None);
    CHECK(workspace.trainable == std::vector<std::string>{"match-a"});
    CHECK(workspace.backbones == std::vector<std::string>{"data/models/dinov2-vitb14-fp16.onnx"});
    CHECK(workspace.detectors == std::vector<std::string>{"weights/rally-detector.onnx",
                                                          "data/models/rally-detector-test.onnx"});
    fs::remove_all(root);
}

TEST_CASE("the process runner collects output and exit codes") {
    const fs::path root = temp_dir();
    touch(root / "hello.txt", "hello from the file\n");
    SubprocessRunner runner;

    runner.start({TTRALLY_CMAKE, {"-E", "cat", "hello.txt"}, root}); // relative to the directory
    wait_for(runner);
    CHECK(runner.exit_code() == 0);
    CHECK(runner.take_output() == "hello from the file\n");
    CHECK(runner.take_output().empty());

    runner.start({TTRALLY_CMAKE, {"-E", "cat", "missing.txt"}, root});
    wait_for(runner);
    CHECK(runner.exit_code() != 0);
    CHECK_FALSE(runner.take_output().empty()); // the error message (stderr is included)

    CHECK_THROWS_AS(runner.start({"no-such-program-ttrally", {}, root}), std::runtime_error);
    fs::remove_all(root);
}

TEST_CASE("a running program can be cancelled") {
    SubprocessRunner runner;
    const auto started = std::chrono::steady_clock::now();
    runner.start({TTRALLY_CMAKE, {"-E", "sleep", "30"}, fs::temp_directory_path()});
    CHECK(runner.running());
    runner.cancel();
    wait_for(runner);
    CHECK_FALSE(runner.running());
    CHECK(runner.exit_code() != 0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
}

TEST_CASE("the preview shows the frame at a share of the video") {
#ifdef _WIN32
    const bool has_ffmpeg = std::system("ffmpeg -hide_banner -version >NUL 2>&1") == 0;
#else
    const bool has_ffmpeg = std::system("ffmpeg -hide_banner -version >/dev/null 2>&1") == 0;
#endif
    if (!has_ffmpeg) {
        SKIP("ffmpeg command-line tool not found");
    }
    const fs::path root = temp_dir();
    fs::create_directories(root);
    const fs::path clip = root / "clip.mp4";
    REQUIRE(std::system(std::format("ffmpeg -hide_banner -loglevel error -y -f lavfi -i "
                                    "testsrc2=size=640x360:rate=30 -t 10 -c:v mpeg4 -g 60 \"{}\"",
                                    clip.string())
                            .c_str()) == 0);
    FfmpegVideoPreview preview;
    const auto middle = preview.frame_at(clip, 0.5, 180);
    REQUIRE(middle);
    CHECK(middle->size.height == 180);
    CHECK(middle->size.width == 320);
    CHECK(middle->time_s == Catch::Approx(4.5).margin(0.05)); // half of 10 s minus 1 s
    CHECK(middle->planes.size() == 320 * 180 * 3 / 2);
    const auto start = preview.frame_at(clip, 0.0, 180);
    REQUIRE(start);
    CHECK(start->time_s < 0.05);
    touch(root / "broken.mp4", "not a video");
    CHECK_FALSE(preview.frame_at(root / "broken.mp4", 0.5, 180));
    fs::remove_all(root);
}

