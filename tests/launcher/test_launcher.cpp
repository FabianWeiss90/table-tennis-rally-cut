// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Forms, command lines and the launcher session with in-memory fakes.

#include "launcher/application/detect_scenes.hpp"
#include "launcher/application/launcher_session.hpp"
#include "launcher/domain/batch.hpp"
#include "launcher/domain/scenes.hpp"
#include "launcher/domain/video_id.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <stdexcept>

using namespace ttrally::launcher;
using Catch::Approx;

namespace {

/// A path in the library as the forms build it (native separators).
std::string in_library(const std::string& relative) {
    return (std::filesystem::path("/lib") / relative).make_preferred().string();
}

VideoEntry video(std::string id) {
    VideoEntry entry;
    entry.match_day = "day-1";
    entry.original = "/lib/day-1/" + id + "_FHD.mp4";
    entry.work_dir = (std::filesystem::path("/lib/day-1") / "ttrally-data").string();
    entry.video_id = std::move(id);
    entry.cut_candidates = {"/lib/day-1/Liimba/" + entry.video_id + "_FHD.mp4"};
    return entry;
}

WorkspaceSnapshot workspace() {
    WorkspaceSnapshot snapshot;
    snapshot.library = "/lib";
    snapshot.videos = {video("match-a"), video("match-b")};
    snapshot.videos[1].detected = true;
    snapshot.videos[1].annotation = AnnotationProgress::Complete;
    snapshot.backbones = {"/lib/ttrally-data/models/dinov2-vitb14.onnx",
                          "/lib/ttrally-data/models/dinov2-vitb14-fp16.onnx"};
    snapshot.detectors = {"weights/rally-detector.onnx"};
    snapshot.trainable = {"day-1/match-b", "day-2/match-c"};
    return snapshot;
}

CommandForm form_named(std::vector<CommandForm> forms, const std::string& name) {
    for (CommandForm& form : forms) {
        if (form.name == name) {
            return form;
        }
    }
    FAIL("no form " << name);
    return {};
}

class FakeScanner final : public WorkspaceScanner {
  public:
    WorkspaceSnapshot scan(const std::filesystem::path& library) override {
        ++scans;
        scanned = library;
        return snapshot;
    }
    WorkspaceSnapshot snapshot = workspace();
    int scans = 0;
    std::filesystem::path scanned;
};

class FakeRunner final : public ProcessRunner {
  public:
    void start(const Invocation& invocation) override {
        started = invocation;
        is_running = true;
    }
    bool running() const override { return is_running; }
    std::string take_output() override { return std::exchange(output, {}); }
    std::optional<int> exit_code() const override {
        return is_running ? std::nullopt : std::optional<int>(code);
    }
    void cancel() override { cancelled = true; }

    Invocation started;
    bool is_running = false;
    bool cancelled = false;
    std::string output;
    int code = 0;
};

class FakePreCutStore final : public PreCutStore {
  public:
    std::map<std::string, std::filesystem::path>
    load(const std::filesystem::path& work_dir) override {
        return stored[work_dir];
    }
    void save(const std::filesystem::path& work_dir,
              const std::map<std::string, std::filesystem::path>& pre_cuts) override {
        stored[work_dir] = pre_cuts;
    }

    std::map<std::filesystem::path, std::map<std::string, std::filesystem::path>> stored;
};

} // namespace

TEST_CASE("video ids are suggested from file names like the existing ones") {
    CHECK(suggest_video_id("Weiss-Heinemeier_FHD") == "weiss-heinemeier");
    CHECK(suggest_video_id("Club Match 2026_4K") == "club-match-2026");
    CHECK(suggest_video_id("FHD") == "fhd"); // nothing else left
    CHECK(suggest_video_id("") == "video");
    CHECK(is_derived_from("Weiss-Heinemeier_FHD_cut_33_rallies", "Weiss-Heinemeier_FHD"));
    CHECK(is_pre_cut_of("4_Weiss-Heinemeier_FHD", "Weiss-Heinemeier_FHD"));
    CHECK(is_pre_cut_of("Weiss-Heinemeier_FHD", "Weiss-Heinemeier_FHD"));
    CHECK_FALSE(is_pre_cut_of("4_Weiss-Handte_FHD", "Weiss-Heinemeier_FHD"));
    CHECK_FALSE(is_derived_from("Weiss-Heinemeier_FHD", "Weiss-Heinemeier_FHD"));
    CHECK_FALSE(is_derived_from("Weiss-Handte_FHD", "Weiss-H"));
}

TEST_CASE("command lines leave out defaults and quote where needed") {
    CommandForm form{"cut", "Cut", "", Program::Ttrally, "", "", {"cut"}, {}};
    form.fields = {
        {"", "Video", FieldKind::File, "data/a b.mp4", "", {}, {}, true, false, false, ""},
        {"--before", "Before", FieldKind::Text, "2", "2", {}, {}, false, false, false, ""},
        {"--after", "After", FieldKind::Text, "3", "2", {}, {}, false, false, false, ""},
        {"--fast", "Fast", FieldKind::Flag, "1", "", {}, {}, false, false, false, ""},
        {"--bitrate", "Bit rate", FieldKind::Text, "", "", {}, {}, false, false, false, ""}};
    CHECK(form.arguments() ==
          std::vector<std::string>{"cut", "data/a b.mp4", "--after", "3", "--fast"});
    CHECK(command_line("ttrally", form.arguments()) ==
          "ttrally cut \"data/a b.mp4\" --after 3 --fast");
    CHECK(form.missing().empty());
    form.fields[0].value.clear();
    CHECK(form.missing() == std::vector<std::string>{"Video"});
}

TEST_CASE("forms are filled in for the selected video, with its files in the work folder") {
    const auto snapshot = workspace();
    auto align = form_named(forms_for(snapshot.videos[0], snapshot), "align");
    CHECK(align.arguments() ==
          std::vector<std::string>{"align", "/lib/day-1/match-a_FHD.mp4",
                                   "/lib/day-1/Liimba/match-a_FHD.mp4", "--out",
                                   in_library("day-1/ttrally-data/align/match-a.csv"), "--report",
                                   in_library("day-1/ttrally-data/align/match-a.html"),
                                   "--cache-dir", in_library("day-1/ttrally-data/cache")});
    auto features = form_named(forms_for(snapshot.videos[0], snapshot), "features");
    CHECK(features.field("--model")->value == "/lib/ttrally-data/models/dinov2-vitb14-fp16.onnx");

    // Cut: complete labels first, then the detections, then labels still in progress
    auto cut_a = form_named(forms_for(snapshot.videos[0], snapshot), "cut");
    CHECK(cut_a.field("--rallies")->value ==
          in_library("day-1/ttrally-data/detections/match-a.csv"));
    auto cut_b = form_named(forms_for(snapshot.videos[1], snapshot), "cut");
    CHECK(cut_b.field("--rallies")->value ==
          in_library("day-1/ttrally-data/annotations/match-b.csv"));
    CHECK(cut_b.field("--rallies")->suggestions ==
          std::vector<std::string>{in_library("day-1/ttrally-data/annotations/match-b.csv"),
                                   in_library("day-1/ttrally-data/detections/match-b.csv")});
    auto in_progress = snapshot;
    in_progress.videos[1].annotation = AnnotationProgress::InProgress;
    auto cut_c = form_named(forms_for(in_progress.videos[1], in_progress), "cut");
    CHECK(cut_c.field("--rallies")->value ==
          in_library("day-1/ttrally-data/detections/match-b.csv"));
    in_progress.videos[1].detected = false;
    auto cut_d = form_named(forms_for(in_progress.videos[1], in_progress), "cut");
    CHECK(cut_d.field("--rallies")->value ==
          in_library("day-1/ttrally-data/annotations/match-b.csv"));
    // Refined detections come before the detections, after complete labels
    auto refined = snapshot;
    refined.videos[0].detected = true;
    refined.videos[0].refinement = AnnotationProgress::InProgress;
    auto cut_e = form_named(forms_for(refined.videos[0], refined), "cut");
    CHECK(cut_e.field("--rallies")->suggestions ==
          std::vector<std::string>{in_library("day-1/ttrally-data/refined/match-a.csv"),
                                   in_library("day-1/ttrally-data/detections/match-a.csv")});
}

TEST_CASE("refine opens the annotation window on the detections") {
    const auto snapshot = workspace();
    auto refine = form_named(forms_for(snapshot.videos[0], snapshot), "refine");
    CHECK(refine.arguments() ==
          std::vector<std::string>{"annotate", "/lib/day-1/match-a_FHD.mp4", "--detections",
                                   in_library("day-1/ttrally-data/detections/match-a.csv"),
                                   "--video-id", "match-a", "--annotations-dir",
                                   in_library("day-1/ttrally-data/refined"), "--cache-dir",
                                   in_library("day-1/ttrally-data/cache")});
    CHECK_FALSE(supports_batch("refine"));
}

TEST_CASE("the training gets the library and the selected videos by match day") {
    const auto snapshot = workspace();
    auto train = form_named(forms_for(snapshot.videos[0], snapshot), "train");
    CHECK(train.program == Program::Uv);
    CHECK(train.working_directory == "training");
    CHECK(train.missing() == std::vector<std::string>{"Training videos"}); // none ticked yet
    CHECK(train.field("--videos")->choices == snapshot.trainable);
    train.field("--videos")->value = "day-2/match-c\n";
    CHECK(train.arguments() ==
          std::vector<std::string>{"run", "python", "-u", "-m", "ttrally_training.train",
                                   "--videos", "day-2/match-c", "--library", "/lib", "--out",
                                   in_library("ttrally-data/models/rally-detector.onnx")});
}

TEST_CASE("the ticked training videos survive choosing another video and rescans") {
    FakeScanner scanner;
    FakeRunner runner;
    LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    auto train = [&]() -> CommandForm& { return session.forms()[6]; };
    REQUIRE(train().name == "train");
    train().field("--videos")->value = "day-1/match-b\nday-2/match-c\n";
    session.select_video(1);
    CHECK(train().field("--videos")->value == "day-1/match-b\nday-2/match-c\n");
    scanner.snapshot.trainable = {"day-2/match-c"}; // match-b got an open gap again
    session.rescan();
    CHECK(train().field("--videos")->value == "day-2/match-c\n");
}

TEST_CASE("the session runs a command and reports its result") {
    FakeScanner scanner;
    FakeRunner runner;
    LauncherSession session(scanner, runner, "/bin/ttrally", "/project", "/lib");
    CHECK(scanner.scanned == "/lib");
    REQUIRE(session.selected_video() == 0);
    auto& cut = session.forms()[5];
    REQUIRE(cut.name == "cut");
    cut.field("--before")->value = "3";

    session.run(cut);
    CHECK(runner.started.program == "/bin/ttrally");
    CHECK(runner.started.working_directory == "/project");
    CHECK(runner.started.arguments[0] == "cut");
    CHECK(session.running());
    CHECK_THROWS_AS(session.run(cut), std::logic_error); // one at a time

    runner.output = "  50 %\n";
    session.poll();
    CHECK(session.log().ends_with("  50 %\n"));
    session.cancel();
    CHECK(runner.cancelled);

    runner.is_running = false;
    runner.code = 1;
    session.poll();
    CHECK_FALSE(session.running());
    CHECK(session.status() == "cut failed (exit code 1)");
    CHECK(scanner.scans == 2); // rescanned after the command
    CHECK(session.forms()[5].field("--before")->value == "3"); // edits survive the rescan
}

TEST_CASE("a pre-cut video chosen in the align form is kept for the video") {
    FakeScanner scanner;
    FakeRunner runner;
    FakePreCutStore pre_cuts;
    LauncherSession session(scanner, runner, "ttrally", "/project", "/lib", &pre_cuts);
    session.select_video(1);
    const std::string work_dir = session.workspace().videos[1].work_dir;
    pre_cuts.stored[work_dir] = {{"another-video", "/x.mp4"}};
    auto& align = session.forms()[0];
    REQUIRE(align.name == "align");

    align.fields[1].value = "/elsewhere/other-name.mp4";
    session.field_edited(align, align.fields[1]);
    CHECK(pre_cuts.stored[work_dir] ==
          std::map<std::string, std::filesystem::path>{
              {"another-video", "/x.mp4"}, {"match-b", "/elsewhere/other-name.mp4"}});
    CHECK(scanner.scans == 2); // shown at once
    CHECK(runner.started.program.empty()); // without running align

    align.fields[1].value = "cuts/relative.mp4"; // as the file dialog gives paths in the project
    session.field_edited(align, align.fields[1]);
    CHECK(pre_cuts.stored[work_dir].at("match-b") == "/project/cuts/relative.mp4");
    align.fields[1].value.clear();
    session.field_edited(align, align.fields[1]);
    CHECK_FALSE(pre_cuts.stored[work_dir].contains("match-b"));
    session.field_edited(align, align.fields[2]); // other fields keep nothing
    CHECK(pre_cuts.stored.size() == 1);
}

TEST_CASE("annotate opens the segments once a pre-cut video is chosen") {
    FakeScanner scanner;
    scanner.snapshot.videos[0].cut_candidates.clear();
    FakeRunner runner;
    FakePreCutStore pre_cuts;
    LauncherSession session(scanner, runner, "ttrally", "/project", "/lib", &pre_cuts);
    CHECK(session.forms()[1].field("--segments")->value.empty());
    scanner.snapshot.videos[0].cut_candidates = {"/elsewhere/a.mp4"}; // as the store makes it
    session.forms()[0].fields[1].value = "/elsewhere/a.mp4";
    session.field_edited(session.forms()[0], session.forms()[0].fields[1]);
    CHECK(session.forms()[1].field("--segments")->value ==
          in_library("day-1/ttrally-data/align/match-a.csv"));
}

TEST_CASE("a batch runs a command for the videos of the match day") {
    FakeScanner scanner;
    scanner.snapshot.videos.push_back(video("match-c"));
    scanner.snapshot.videos.push_back(video("match-d"));
    scanner.snapshot.videos[3].match_day = "day-2"; // another match day: left out
    scanner.snapshot.videos[0].aligned = true;
    scanner.snapshot.videos[2].cut_candidates.clear(); // no pre-cut: skipped
    FakeRunner runner;
    LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    auto& align = session.forms()[0];
    align.field("--min-confidence")->value = "3";       // the same for every video: carried over
    align.field("--out")->value = "/edited/match-a.csv"; // depends on the video: not carried over

    SECTION("only the videos without a result") {
        session.run_batch(align, BatchScope::Missing);
        CHECK(runner.started.arguments[1] == "/lib/day-1/match-b_FHD.mp4");
        CHECK(session.log().find("1 already done") != std::string::npos);
        CHECK(session.log().find("Skipped: match-c (no pre-cut video)") != std::string::npos);
        runner.is_running = false;
        session.poll();
        CHECK_FALSE(session.running());
        CHECK(session.status() == "align finished for 1 videos");
    }
    SECTION("all again; a failed video does not stop the others") {
        session.run_batch(align, BatchScope::All);
        const auto first = runner.started.arguments;
        CHECK(first[1] == "/lib/day-1/match-a_FHD.mp4");
        CHECK(std::ranges::find(first, "/edited/match-a.csv") != first.end());
        CHECK_THROWS_AS(session.run(align), std::logic_error);
        runner.is_running = false;
        runner.code = 2;
        session.poll();
        REQUIRE(session.running());
        const auto second = runner.started.arguments;
        CHECK(second[1] == "/lib/day-1/match-b_FHD.mp4");
        CHECK(std::ranges::find(second, in_library("day-1/ttrally-data/align/match-b.csv")) !=
              second.end());
        const auto confidence = std::ranges::find(second, "--min-confidence");
        REQUIRE(confidence != second.end());
        CHECK(*std::next(confidence) == "3");
        runner.is_running = false;
        runner.code = 0;
        session.poll();
        CHECK_FALSE(session.running());
        CHECK(session.status() == "align failed for 1 of 2 videos: match-a");
    }
    SECTION("cancelling stops the rest") {
        session.run_batch(align, BatchScope::All);
        session.cancel();
        runner.is_running = false;
        runner.code = -15;
        session.poll();
        CHECK_FALSE(session.running());
        CHECK(session.status() ==
              "align failed for 2 of 2 videos: match-b (cancelled), match-a");
    }
}

TEST_CASE("a batch computes the features again only when all are asked for") {
    FakeScanner scanner;
    FakeRunner runner;
    LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    auto& features = session.forms()[2];
    REQUIRE(features.name == "features");
    session.run_batch(features, BatchScope::All);
    CHECK(std::ranges::find(runner.started.arguments, "--force") != runner.started.arguments.end());
    session.cancel();
    runner.is_running = false;
    session.poll();
    session.run_batch(features, BatchScope::Missing);
    CHECK(std::ranges::find(runner.started.arguments, "--force") == runner.started.arguments.end());
    CHECK_THROWS_AS(session.run_batch(session.forms()[1], BatchScope::All), std::logic_error);
}

TEST_CASE("which steps a batch can run, has done and lacks the input for") {
    VideoEntry entry = video("match-a");
    CHECK(supports_batch("cut"));
    CHECK_FALSE(supports_batch("annotate"));
    CHECK_FALSE(supports_batch("train"));
    CHECK_FALSE(step_done("detect", entry));
    entry.detected = true;
    CHECK(step_done("detect", entry));
    CHECK(missing_input("align", entry).empty());
    CHECK(missing_input("cut", entry).empty()); // detections
    entry.detected = false;
    CHECK(missing_input("cut", entry) == "no labels or detections");
    entry.cut_candidates.clear();
    CHECK(missing_input("align", entry) == "no pre-cut video");
    CHECK(missing_input("features", entry).empty());
}

TEST_CASE("a form with empty required fields is not run") {
    FakeScanner scanner;
    scanner.snapshot.videos[0].cut_candidates.clear();
    FakeRunner runner;
    LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    CHECK_THROWS_AS(session.run(session.forms()[0]), std::logic_error); // no pre-cut video
    CHECK_FALSE(runner.is_running);
    session.select_video(1);
    CHECK(session.forms()[0].field("--out")->value ==
          in_library("day-1/ttrally-data/align/match-b.csv"));
}

TEST_CASE("switching the library scans it") {
    FakeScanner scanner;
    FakeRunner runner;
    LauncherSession session(scanner, runner, "ttrally", ".", "/lib");
    session.select_video(1);
    scanner.snapshot.videos.pop_back();
    session.set_library("/other");
    CHECK(session.library() == "/other");
    CHECK(scanner.scanned == "/other");
    CHECK(session.selected_video() == 0);
    session.run(session.forms()[1]);
    CHECK_THROWS_AS(session.set_library("/lib"), std::logic_error);
}

TEST_CASE("scenes are grouped from the similarity of the backgrounds") {
    // Measured on one match day: four games from one camera position, one from another
    const std::vector<std::vector<double>> similarities{{1.00, 0.91, 0.88, 0.92, 0.41},
                                                        {0.91, 1.00, 0.87, 0.96, 0.40},
                                                        {0.88, 0.87, 1.00, 0.87, 0.43},
                                                        {0.92, 0.96, 0.87, 1.00, 0.40},
                                                        {0.41, 0.40, 0.43, 0.40, 1.00}};
    CHECK(group_into_scenes(similarities) ==
          std::vector<std::string>{"A", "A", "A", "A", "B"});
    CHECK(group_into_scenes({{1.0, 0.9, 0.2}, {0.9, 1.0, 0.2}, {0.2, 0.2, 1.0}}, 0.95) ==
          std::vector<std::string>{"A", "B", "C"});
    CHECK(group_into_scenes({}).empty());
    CHECK(scene_name(0) == "A");
    CHECK(scene_name(25) == "Z");
    CHECK(scene_name(26) == "AA");
}

TEST_CASE("backgrounds are compared independent of brightness") {
    const Background a{2, 2, {0, 10, 20, 30}};
    const Background brighter{2, 2, {50, 70, 90, 110}};
    const Background mirrored{2, 2, {30, 20, 10, 0}};
    CHECK(similarity(a, brighter) == Approx(1.0));
    CHECK(similarity(a, mirrored) == Approx(-1.0));
    CHECK(similarity(a, Background{4, 1, a.pixels}) == 0.0); // other size
    CHECK(similarity(a, Background{2, 2, {5, 5, 5, 5}}) == 0.0); // uniform
}

TEST_CASE("a scene in training and validation is reported") {
    auto snapshot = workspace();
    snapshot.videos.push_back(video("match-c"));
    snapshot.videos[0].scene = "A";
    snapshot.videos[1].scene = "A";
    snapshot.videos[2].scene = "B";
    const std::vector<std::string> trainable{"day-1/match-a", "day-1/match-b", "day-1/match-c"};
    CHECK(scenes_in_both(snapshot.videos, trainable, {"day-1/match-a"}) ==
          std::vector<std::string>{"day-1, scene A"});
    CHECK(scenes_in_both(snapshot.videos, trainable, {"day-1/match-a", "day-1/match-b"}).empty());
    CHECK(scenes_in_both(snapshot.videos, {"day-1/match-a", "day-1/match-c"},
                         {"day-1/match-a"})
              .empty()); // match-b cannot be trained on, so it validates nothing
}

TEST_CASE("the scenes of videos are detected from frames spread over them") {
    // Two hall pictures; the "players" (one bright pixel) move from frame to frame
    class Halls final : public VideoPreview {
      public:
        std::optional<ttrally::media::VideoFrame> frame_at(const std::filesystem::path& video,
                                                           double fraction, int) override {
            if (video == "broken.mp4") {
                return std::nullopt;
            }
            ttrally::media::VideoFrame frame;
            frame.size = {8, 4};
            frame.planes.assign(ttrally::media::VideoFrame::bytes_for(frame.size), 128);
            for (int i = 0; i < 32; ++i) {
                const bool left_hall = video.string().starts_with("left");
                frame.planes[i] = static_cast<std::uint8_t>(left_hall ? i * 4 : 255 - i * i / 4);
            }
            frame.planes[static_cast<std::size_t>(fraction * 31)] = 255;
            return frame;
        }
    } halls;
    CHECK(video_background(halls, "broken.mp4").pixels.empty());
    CHECK(video_background(halls, "left-1.mp4").pixels.size() == 32);
    CHECK(detect_scenes(halls, {"left-1.mp4", "right-1.mp4", "left-2.mp4", "broken.mp4"}) ==
          std::vector<std::string>{"A", "B", "A", "C"});
}

TEST_CASE("a video without a pre-cut version is annotated without segments") {
    auto snapshot = workspace();
    snapshot.videos[0].cut_candidates.clear();
    auto manual = form_named(forms_for(snapshot.videos[0], snapshot), "annotate");
    CHECK(manual.field("--segments")->value.empty());
    CHECK(manual.missing().empty());
    const auto arguments = manual.arguments();
    CHECK(std::ranges::find(arguments, "--segments") == arguments.end());
    CHECK(manual.field("--video-id")->value == "match-a");

    auto aligned = form_named(forms_for(snapshot.videos[1], snapshot), "annotate");
    CHECK(aligned.field("--segments")->value ==
          in_library("day-1/ttrally-data/align/match-b.csv"));
}
