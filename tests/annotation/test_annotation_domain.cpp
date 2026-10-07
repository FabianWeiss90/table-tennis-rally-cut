// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/domain/annotation_sheet.hpp"
#include "annotation/domain/review_plan.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ttrally::annotation;

namespace {

AnnotationSheet empty_sheet() { return {"match_a", {60000, 1001}, 10000}; }

RallyLabel rally(std::int64_t start, std::int64_t end) { return {start, end, {}, false, ""}; }

} // namespace

TEST_CASE("rallies are kept in order and numbered by position") {
    auto sheet = empty_sheet();
    CHECK(sheet.add(rally(500, 600)) == 1);
    CHECK(sheet.add(rally(100, 200)) == 1); // inserted before the first one
    CHECK(sheet.add(rally(300, 350)) == 2);
    REQUIRE(sheet.rallies().size() == 3);
    CHECK(sheet.rally(1).start_frame == 100);
    CHECK(sheet.rally(3).start_frame == 500);
    CHECK(sheet.rally_at(320) == 2);
    CHECK_FALSE(sheet.rally_at(250));
}

TEST_CASE("annotation rules are enforced") {
    auto sheet = empty_sheet();
    sheet.add(rally(100, 200));
    CHECK_THROWS_AS(sheet.add(rally(150, 250)), AnnotationRuleViolation); // overlap
    CHECK_THROWS_AS(sheet.add(rally(200, 300)), AnnotationRuleViolation); // shares frame 200
    CHECK_THROWS_AS(sheet.add(rally(400, 300)), AnnotationRuleViolation); // end before start
    CHECK_THROWS_AS(sheet.add(rally(9990, 10000)), AnnotationRuleViolation); // past the end
    RallyLabel serve_outside = rally(400, 500);
    serve_outside.serve_contact_frame = 600;
    CHECK_THROWS_AS(sheet.add(serve_outside), AnnotationRuleViolation);
    RallyLabel line_break = rally(400, 500);
    line_break.notes = "two\nlines";
    CHECK_THROWS_AS(sheet.add(line_break), AnnotationRuleViolation);
    RallyLabel both = rally(400, 500);
    both.aborted_toss = true;
    both.let = true;
    CHECK_THROWS_AS(sheet.add(both), AnnotationRuleViolation);
    CHECK(sheet.add(rally(201, 201)) == 2); // a one-frame aborted toss is allowed
}

TEST_CASE("replacing a rally may move it and ignores its old position") {
    auto sheet = empty_sheet();
    sheet.add(rally(100, 200));
    sheet.add(rally(300, 400));
    CHECK(sheet.replace(1, rally(150, 260)) == 1);   // overlaps only its old self
    CHECK(sheet.replace(1, rally(500, 600)) == 2);   // moved behind the other one
    CHECK(sheet.rally(1).start_frame == 300);
    sheet.remove(1);
    CHECK(sheet.rallies().size() == 1);
}

TEST_CASE("review items are ordered by frame and derive their status from the rallies") {
    ReviewPlan plan({{ReviewKind::Candidate, 1, 700, 900},
                     {ReviewKind::Gap, 1, 0, 699},
                     {ReviewKind::Candidate, 2, 1500, 1800}});
    REQUIRE(plan.items().size() == 3);
    CHECK(plan.item(0).kind == ReviewKind::Gap);
    CHECK(plan.first_open() == 0);

    auto sheet = empty_sheet();
    sheet.add(rally(720, 880));
    plan.refresh(sheet);
    CHECK(plan.item(1).status == ReviewStatus::Annotated);
    CHECK(plan.next_open(0) == 2);

    plan.set_manual_status(0, ReviewStatus::Reviewed);
    plan.set_manual_status(2, ReviewStatus::Rejected);
    CHECK_FALSE(plan.first_open());
    CHECK_THROWS_AS(plan.set_manual_status(0, ReviewStatus::Rejected), std::invalid_argument);

    const auto progress = plan.progress();
    CHECK(progress.candidates == 2);
    CHECK(progress.candidates_done == 2);
    CHECK(progress.gaps_done == 1);

    sheet.remove(1);
    plan.refresh(sheet);
    CHECK(plan.item(1).status == ReviewStatus::Open);
}
