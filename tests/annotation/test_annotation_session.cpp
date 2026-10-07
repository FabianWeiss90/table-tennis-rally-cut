// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/application/annotation_session.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ttrally::annotation;

namespace {

class MemoryAnnotations final : public AnnotationRepository {
  public:
    std::vector<RallyLabel> load(const std::string& /*video_id*/) override { return saved; }
    void save(const AnnotationSheet& sheet) override {
        saved = sheet.rallies();
        ++saves;
    }
    std::vector<RallyLabel> saved;
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
    CHECK(session.save_draft_if_complete() == 1);
    CHECK(fixture.annotations.saved.size() == 1);
    CHECK(session.plan().item(2).status == ReviewStatus::Annotated); // the gap now has a rally
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
