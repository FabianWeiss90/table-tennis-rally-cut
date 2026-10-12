// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// Forms, command lines and the launcher session with in-memory fakes.

#include "launcher/application/launcher_session.hpp"
#include "launcher/domain/video_id.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace ttrally::launcher;

namespace {

VideoEntry video(std::string id) {
    VideoEntry entry;
    entry.original = "data/" + id + "_FHD.mp4";
    entry.video_id = std::move(id);
    entry.cut_candidates = {entry.original.substr(0, entry.original.size() - 4) + "_cut.mp4"};
    return entry;
}

WorkspaceSnapshot workspace() {
    WorkspaceSnapshot snapshot;
    snapshot.videos = {video("match-a"), video("match-b")};
    snapshot.videos[1].detected = true;
    snapshot.videos[1].annotation = AnnotationProgress::Complete;
    snapshot.backbones = {"data/models/dinov2-vitb14.onnx", "data/models/dinov2-vitb14-fp16.onnx"};
    snapshot.detectors = {"weights/rally-detector.onnx"};
    snapshot.trainable = {"match-b", "match-c"};
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
    WorkspaceSnapshot scan() override {
        ++scans;
        return snapshot;
    }
    WorkspaceSnapshot snapshot = workspace();
    int scans = 0;
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

} // namespace

TEST_CASE("video ids are suggested from file names like the existing ones") {
    CHECK(suggest_video_id("Weiss-Heinemeier_FHD") == "weiss-heinemeier");
    CHECK(suggest_video_id("Club Match 2026_4K") == "club-match-2026");
    CHECK(suggest_video_id("FHD") == "fhd"); // nothing else left
    CHECK(suggest_video_id("") == "video");
    CHECK(is_derived_from("Weiss-Heinemeier_FHD_cut_33_rallies", "Weiss-Heinemeier_FHD"));
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

TEST_CASE("forms are filled in for the selected video") {
    const auto snapshot = workspace();
    auto align = form_named(forms_for(snapshot.videos[0], snapshot), "align");
    CHECK(align.arguments() ==
          std::vector<std::string>{"align", "data/match-a_FHD.mp4", "data/match-a_FHD_cut.mp4",
                                   "--out", "data/align/match-a.csv", "--report",
                                   "data/align/match-a.html"});
    auto features = form_named(forms_for(snapshot.videos[0], snapshot), "features");
    CHECK(features.field("--model")->value == "data/models/dinov2-vitb14-fp16.onnx");

    // Cut: detections if there are any, else the labels
    auto cut_a = form_named(forms_for(snapshot.videos[0], snapshot), "cut");
    CHECK(cut_a.field("--rallies")->value == "data/detections/match-a.csv");
    auto cut_b = form_named(forms_for(snapshot.videos[1], snapshot), "cut");
    CHECK(cut_b.field("--rallies")->suggestions ==
          std::vector<std::string>{"data/detections/match-b.csv",
                                   "data/annotations/match-b.csv"});
}

TEST_CASE("the training runs in training/ with paths relative to it") {
    const auto snapshot = workspace();
    auto train = form_named(forms_for(snapshot.videos[0], snapshot), "train");
    CHECK(train.program == Program::Uv);
    CHECK(train.working_directory == "training");
    train.field("--groups")->value = "data/annotations/groups.csv";
    CHECK(train.arguments() ==
          std::vector<std::string>{"run", "python", "-u", "-m", "ttrally_training.train",
                                   "--videos", "match-b", "match-c", "--groups",
                                   "../data/annotations/groups.csv"});
}

TEST_CASE("the session runs a command and reports its result") {
    FakeScanner scanner;
    FakeRunner runner;
    LauncherSession session(scanner, runner, "/bin/ttrally", "/project");
    REQUIRE(session.selected_video() == 0);
    auto& cut = session.forms()[4];
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
    CHECK(session.forms()[4].field("--before")->value == "3"); // edits survive the rescan
}

TEST_CASE("a form with empty required fields is not run") {
    FakeScanner scanner;
    scanner.snapshot.videos[0].cut_candidates.clear();
    FakeRunner runner;
    LauncherSession session(scanner, runner, "ttrally", ".");
    CHECK_THROWS_AS(session.run(session.forms()[0]), std::logic_error); // no pre-cut video
    CHECK_FALSE(runner.is_running);
    session.select_video(1);
    CHECK(session.forms()[0].field("--out")->value == "data/align/match-b.csv");
}
