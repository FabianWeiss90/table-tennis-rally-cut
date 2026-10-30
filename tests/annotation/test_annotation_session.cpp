// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/application/annotation_session.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ttrally::annotation;

namespace {

class MemoryAnnotations final : public AnnotationRepository {
  public:
    StoredLabels load(const std::string& /*video_id*/) override { return {saved, ignored}; }
    void save(const AnnotationSheet& sheet) override {
        saved = sheet.rallies();
        ignored = sheet.ignored_sections();
        ++saves;
    }
    std::vector<RallyLabel> saved;
    std::vector<IgnoredSection> ignored;
    int saves = 0;
};

class FixedItems final : public ReviewItemSource {
  public:
    std::vector<ReviewItem> load() override {
        return {{ReviewKind::Gap, 1, 0, 599},
                {ReviewKind::Candidate, 1, 600, 900},
                {ReviewKind::Gap, 2, 901, 1999},
                {ReviewKind::Candidate, 2, 2000, 2300}};
    }
};

class MemoryStates final : public ReviewStateStore {
  public:
    ReviewStatusMap load(const std::string& /*video_id*/) override { return loaded; }
    void save(const std::string& /*video_id*/, const std::vector<ReviewItem>& items) override {
        saved = items;
    }
    ReviewStatusMap loaded;
    std::vector<ReviewItem> saved;
};

struct Fixture {
    MemoryAnnotations annotations;
    FixedItems items;
    MemoryStates states;

    AnnotationSession open() {
        return {{"match_a", {60, 1}, 5000}, annotations, items, states};
    }
};

} // namespace

TEST_CASE("a marked rally is saved immediately and completes its candidate") {
    Fixture fixture;
    auto session = fixture.open();
    REQUIRE(session.current_item() == 0); // the first gap

    REQUIRE(session.select_next_open());
    CHECK(session.plan().item(*session.current_item()).kind == ReviewKind::Candidate);
    session.mark_start(640);
    session.mark_serve_contact(660);
    session.mark_end(870);
    CHECK(session.save_draft() == 1);

    REQUIRE(fixture.annotations.saved.size() == 1);
    CHECK(fixture.annotations.saved[0].serve_contact_frame == 660);
    CHECK(session.plan().item(1).status == ReviewStatus::Annotated);
    CHECK(session.draft().empty());
}

TEST_CASE("after a rally the next segment without a rally is selected, skipping gaps") {
    Fixture fixture;
    auto session = fixture.open();
    session.select_item(1); // segment 1
    session.mark_start(640);
    session.mark_end(870);
    session.save_draft();
    REQUIRE(session.select_next_open_candidate());
    CHECK(session.current_item() == 3); // segment 2, gap 2 skipped
    CHECK_FALSE(session.select_next_open_candidate()); // no further segment
}

TEST_CASE("only complete drafts are saved automatically") {
    Fixture fixture;
    auto session = fixture.open();
    session.mark_start(1200); // anywhere, here inside gap 2
    CHECK_FALSE(session.save_draft_if_complete());
    CHECK(fixture.annotations.saves == 0);
    session.mark_end(1300);
    const auto saved = session.save_draft_if_complete();
    REQUIRE(saved);
    CHECK(saved->id == 1);
    CHECK_FALSE(saved->ignored);
    CHECK(fixture.annotations.saved.size() == 1);
    // A rally found in a gap does not complete it: the rest of the gap may hide more
    CHECK(session.plan().item(2).status == ReviewStatus::Open);
    session.select_item(2);
    session.mark_current_reviewed();
    CHECK(session.plan().item(2).status == ReviewStatus::Reviewed);
}

TEST_CASE("an incomplete or invalid draft is not saved") {
    Fixture fixture;
    auto session = fixture.open();
    session.mark_start(100);
    CHECK_THROWS_AS(session.save_draft(), AnnotationRuleViolation);
    session.mark_end(50);
    CHECK_THROWS_AS(session.save_draft(), AnnotationRuleViolation);
    CHECK(fixture.annotations.saves == 0);
    CHECK(session.draft().start_frame == 100); // the draft is kept for correction
}

TEST_CASE("saved rallies can be edited and deleted") {
    Fixture fixture;
    fixture.annotations.saved = {{640, 870, {}, false, ""}, {2010, 2250, {}, false, ""}};
    auto session = fixture.open();
    CHECK(session.sheet().rallies().size() == 2);

    session.edit_rally(2);
    session.set_aborted_toss(true);
    session.set_notes("let");
    CHECK(session.save_draft() == 2);
    CHECK(fixture.annotations.saved[1].aborted_toss);
    CHECK(fixture.annotations.saved[1].notes == "let");

    session.delete_rally(1);
    REQUIRE(fixture.annotations.saved.size() == 1);
    CHECK(session.plan().item(1).status == ReviewStatus::Open);
}

TEST_CASE("segments without a rally are listed until annotated or rejected") {
    Fixture fixture;
    auto session = fixture.open();
    CHECK(session.segments_without_rally() == std::vector<std::size_t>{1, 3});
    session.mark_start(700);
    session.mark_end(800);
    session.save_draft();
    session.select_item(3);
    session.reject_current();
    CHECK(session.segments_without_rally().empty());
}

TEST_CASE("the rally at a frame can be deleted") {
    Fixture fixture;
    fixture.annotations.saved = {{640, 870, {}, false, ""}};
    auto session = fixture.open();
    CHECK_FALSE(session.delete_rally_at(900));
    CHECK(session.delete_rally_at(700) == 1);
    CHECK(fixture.annotations.saved.empty());
}

TEST_CASE("review progress is stored and restored") {
    Fixture fixture;
    {
        auto session = fixture.open();
        session.mark_current_reviewed(); // gap 1
        session.select_item(1);
        session.reject_current(); // candidate 1
    }
    // Every item is saved with its frames and status, open ones included
    REQUIRE(fixture.states.saved.size() == 4);
    CHECK(fixture.states.saved[0].status == ReviewStatus::Reviewed);
    CHECK(fixture.states.saved[1].status == ReviewStatus::Rejected);
    CHECK(fixture.states.saved[2].status == ReviewStatus::Open);
    CHECK(fixture.states.saved[3].first_frame == 2000);
    for (const auto& item : fixture.states.saved) {
        fixture.states.loaded[{item.kind, item.source_id}] = item.status;
    }

    auto session = fixture.open();
    CHECK(session.plan().item(0).status == ReviewStatus::Reviewed);
    CHECK(session.plan().item(1).status == ReviewStatus::Rejected);
    CHECK(session.current_item() == 2); // first open item
    REQUIRE(session.select_previous_open() == false);
}

TEST_CASE("a video is complete once every segment and gap is done") {
    Fixture fixture;
    auto session = fixture.open();
    CHECK_FALSE(session.plan().progress().complete());
    session.mark_current_reviewed(); // gap 1
    session.select_item(1);
    session.mark_start(640);
    session.mark_end(870);
    session.save_draft(); // candidate 1
    CHECK(fixture.states.saved[1].status == ReviewStatus::Annotated); // saved with the rally
    session.select_item(2);
    session.mark_current_reviewed(); // gap 2
    CHECK(session.unchecked_gaps().empty());
    CHECK_FALSE(session.plan().progress().complete());
    session.select_item(3);
    session.reject_current(); // candidate 2
    CHECK(session.plan().progress().complete());
}

TEST_CASE("after checking a gap the next open gap is selected") {
    Fixture fixture;
    auto session = fixture.open();
    session.select_item(2); // gap 2, gap 1 is still open
    session.mark_current_reviewed();
    REQUIRE(session.select_next_open_gap());
    CHECK(session.current_item() == 0); // continues from the start
    session.mark_current_reviewed();
    CHECK_FALSE(session.select_next_open_gap()); // every gap is done
    CHECK(session.current_item() == 0);
}

TEST_CASE("a status for the wrong kind of item is refused with a hint") {
    Fixture fixture;
    auto session = fixture.open();
    session.select_item(1); // a segment
    CHECK_THROWS_AS(session.mark_current_reviewed(), AnnotationRuleViolation);
    session.select_item(0); // a gap
    CHECK_THROWS_AS(session.reject_current(), AnnotationRuleViolation);
    CHECK(session.plan().item(0).status == ReviewStatus::Open);
}

TEST_CASE("marks can be saved as an ignored section that completes the item") {
    Fixture fixture;
    auto session = fixture.open(); // gap 1 (0-599)
    session.mark_start(0);
    session.mark_end(599); // all of the gap; a part would leave it open
    session.set_notes("recording started mid-rally");
    CHECK(session.save_draft_as_ignored() == 1);
    REQUIRE(fixture.annotations.ignored.size() == 1);
    CHECK(fixture.annotations.saved.empty()); // not a rally
    CHECK(session.plan().item(0).status == ReviewStatus::Ignored);
    CHECK(fixture.states.saved[0].status == ReviewStatus::Ignored);

    CHECK(session.delete_ignored_at(200) == 1);
    CHECK(fixture.annotations.ignored.empty());
    CHECK(session.plan().item(0).status == ReviewStatus::Open);
}

TEST_CASE("an ignored section has no rally flags") {
    Fixture fixture;
    auto session = fixture.open();
    session.mark_start(0);
    session.mark_end(450);
    session.set_let(true);
    CHECK_THROWS_AS(session.save_draft_as_ignored(), AnnotationRuleViolation);
    CHECK(fixture.annotations.ignored.empty());
}

TEST_CASE("a saved rally can become an ignored section and back") {
    Fixture fixture;
    fixture.annotations.saved = {{640, 870, {}, false, ""}};
    auto session = fixture.open();
    session.edit_rally(1);
    CHECK(session.save_draft_as_ignored() == 1);
    CHECK(fixture.annotations.saved.empty());
    REQUIRE(fixture.annotations.ignored.size() == 1);

    session.edit_ignored(1);
    CHECK(session.save_draft_if_complete()->ignored); // editing keeps the kind
    session.edit_ignored(1);
    CHECK(session.save_draft() == 1); // Enter makes it a rally again
    CHECK(fixture.annotations.ignored.empty());
    CHECK(fixture.annotations.saved.size() == 1);
}


TEST_CASE("a video without alignment is annotated by hand in one gap over all of it") {
    Fixture fixture;
    WholeVideoReviewSource whole_video(5000);
    AnnotationSession session({"match_a", {60, 1}, 5000}, fixture.annotations, whole_video,
                              fixture.states);
    REQUIRE(session.plan().items().size() == 1);
    CHECK(session.plan().item(0).title() == "Whole video");
    CHECK(session.plan().item(0).last_frame == 4999);
    CHECK(session.current_item() == 0);

    session.mark_start(1200);
    session.mark_end(1300);
    CHECK(session.save_draft() == 1);
    session.mark_start(3000);
    session.mark_end(3400);
    CHECK(session.save_draft() == 2);
    CHECK_FALSE(session.plan().progress().complete()); // not looked at to the end yet
    CHECK(session.entry_frame(0) == 3401); // reopened later, it continues after the last rally

    session.select_item(0);
    session.mark_current_reviewed();
    CHECK(session.plan().progress().complete());
    CHECK(fixture.annotations.saved.size() == 2);
}

TEST_CASE("an edge of a saved rally can be moved") {
    Fixture fixture;
    fixture.annotations.saved = {{640, 870, 660, false, ""}};
    auto session = fixture.open();
    CHECK(session.move_rally_edge(1, Edge::End, 890) == 1);
    CHECK(fixture.annotations.saved[0].end_frame == 890);
    session.move_rally_edge(1, Edge::Start, 620);
    CHECK(fixture.annotations.saved[0].start_frame == 620);
    CHECK_THROWS_AS(session.move_rally_edge(1, Edge::Start, 900), AnnotationRuleViolation);
    CHECK(fixture.annotations.saved[0].start_frame == 620); // unchanged after a refused move
}

TEST_CASE("moving an edge onto neighbouring rallies merges them") {
    Fixture fixture;
    fixture.annotations.saved = {{100, 200, {}, false, "first"},
                                 {300, 400, 320, false, ""},
                                 {450, 500, {}, false, "third"},
                                 {700, 800, {}, false, ""}};
    auto session = fixture.open();
    // The end of the first is dragged into the third: the second lies between them
    CHECK(session.move_rally_edge(1, Edge::End, 460) == 1);
    REQUIRE(fixture.annotations.saved.size() == 2);
    const RallyLabel& merged = fixture.annotations.saved[0];
    CHECK(merged.start_frame == 100);
    CHECK(merged.end_frame == 500); // the third reaches further than the drop
    CHECK(merged.serve_contact_frame == 320);
    CHECK(merged.notes == "first; third");
    CHECK(fixture.annotations.saved[1].start_frame == 700);
    // Moving the start of the last backwards onto the merged one merges them as well
    CHECK(session.move_rally_edge(2, Edge::Start, 480) == 1);
    CHECK(fixture.annotations.saved.size() == 1);
    CHECK(fixture.annotations.saved[0].end_frame == 800);
}

TEST_CASE("a rally is not merged into an ignored section") {
    Fixture fixture;
    fixture.annotations.saved = {{100, 200, {}, false, ""}};
    fixture.annotations.ignored = {{250, 300, ""}};
    auto session = fixture.open();
    CHECK_THROWS_AS(session.move_rally_edge(1, Edge::End, 260), AnnotationRuleViolation);
    CHECK(session.move_ignored_edge(1, Edge::Start, 210) == 1);
    CHECK(fixture.annotations.ignored[0].start_frame == 210);
}
