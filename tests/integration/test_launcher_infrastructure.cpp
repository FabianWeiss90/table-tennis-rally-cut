// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// The library scanner on a small library, and the process runner with the cmake
// command-line tool as a portable test program.

#include "launcher/infrastructure/csv_scene_store.hpp"
#include "launcher/infrastructure/ffmpeg_video_preview.hpp"
#include "launcher/infrastructure/library_scanner.hpp"
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

TEST_CASE("the scanner finds the match days, their videos, pre-cuts and progress") {
    const fs::path library = temp_dir();
    const fs::path day = library / "2026-10-03 Club";
    const fs::path work = day / "ttrally-data";
    touch(day / "Match-A_FHD.MP4");
    touch(day / "Match-B.mov");
    touch(day / "notes.txt");
    touch(day / "Liimba-Cut" / "3_Match-A_FHD.mp4");
    touch(day / "Liimba-Cut" / "4_Match-C_FHD.mp4");
    touch(work / "align" / "match-a.csv");
    touch(work / "annotations" / "match-a.csv",
          "video_id,rally_id,start_frame,end_frame,fps,serve_contact_frame,flags,notes\n"
          "match-a,1,10,20,60,,,\nmatch-a,,30,40,60,,ignore,\n");
    touch(work / "annotations" / "match-a.review.csv",
          "kind,id,first_frame,last_frame,status\ngap,1,0,9,reviewed\ncandidate,1,10,20,annotated\n");
    touch(work / "features" / "match-a" / "manifest.json",
          R"json({"model": "facebook/dinov2-base (fp16)"})json");
    touch(work / "cuts" / "match-a.mp4");
    touch(work / "cache" / "x.f32");
    touch(library / "2026-10-10 Away" / "Match-D.mp4");
    touch(library / ".hidden" / "Match-E.mp4");
    touch(library / "Exclude" / "Match-F.mp4");
    touch(library / "ttrally-data" / "models" / "dinov2-vitb14-fp16.onnx");
    touch(library / "ttrally-data" / "models" / "rally-detector-test.onnx");
    const fs::path project = temp_dir();
    touch(project / "weights" / "rally-detector.onnx");

    const WorkspaceSnapshot workspace = LibraryScanner(project).scan(library);
    CHECK(fs::path(workspace.library) == fs::absolute(library).lexically_normal());
    REQUIRE(workspace.videos.size() == 3);
    const VideoEntry& a = workspace.videos[0];
    CHECK(a.match_day == "2026-10-03 Club");
    CHECK(fs::path(a.original) == day / "Match-A_FHD.MP4");
    CHECK(fs::path(a.work_dir) == work);
    CHECK(a.video_id == "match-a");
    CHECK(a.key() == "2026-10-03 Club/match-a");
    REQUIRE(a.cut_candidates.size() == 1);
    CHECK(fs::path(a.cut_candidates[0]) == day / "Liimba-Cut" / "3_Match-A_FHD.mp4");
    CHECK(a.aligned);
    CHECK(a.annotation == AnnotationProgress::Complete);
    CHECK(a.rallies == 1); // the ignored section is no rally
    CHECK(a.features_model == "facebook/dinov2-base (fp16)");
    CHECK_FALSE(a.detected);
    REQUIRE(a.cuts.size() == 1);
    CHECK(fs::path(a.cuts[0]) == work / "cuts" / "match-a.mp4");
    CHECK(workspace.videos[1].video_id == "match-b");
    CHECK(workspace.videos[1].annotation == AnnotationProgress::None);
    CHECK(workspace.videos[1].cut_candidates.empty());
    CHECK(workspace.videos[2].match_day == "2026-10-10 Away");
    CHECK(workspace.trainable == std::vector<std::string>{"2026-10-03 Club/match-a"});
    REQUIRE(workspace.backbones.size() == 1);
    CHECK(fs::path(workspace.backbones[0]).filename() == "dinov2-vitb14-fp16.onnx");
    REQUIRE(workspace.detectors.size() == 2);
    CHECK(fs::path(workspace.detectors[0]).filename() == "rally-detector-test.onnx");
    CHECK(fs::path(workspace.detectors[1]) == project / "weights" / "rally-detector.onnx");
    fs::remove_all(library);
    fs::remove_all(project);
}

TEST_CASE("scenes are stored per match day and read by the scanner") {
    const fs::path library = temp_dir();
    const fs::path day = library / "day";
    touch(day / "Match-A.mp4");
    touch(day / "Match-B.mp4");
    CsvSceneStore store;
    CHECK(store.load(day / "ttrally-data").empty());
    store.save(day / "ttrally-data", {{"match-a", "B"}, {"match, odd", "A"}});
    CHECK(store.load(day / "ttrally-data") ==
          std::map<std::string, std::string>{{"match-a", "B"}, {"match, odd", "A"}});
    const WorkspaceSnapshot workspace = LibraryScanner(library).scan(library);
    REQUIRE(workspace.videos.size() == 2);
    CHECK(workspace.videos[0].scene == "B");
    CHECK(workspace.videos[1].scene.empty());
    fs::remove_all(library);
}

TEST_CASE("the launcher remembers the library") {
    const fs::path root = temp_dir();
    FileLauncherSettings settings(root / "config" / "launcher.txt");
    CHECK_FALSE(settings.library());
    settings.save_library(root / "My Videos");
    CHECK(settings.library() == root / "My Videos");
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

