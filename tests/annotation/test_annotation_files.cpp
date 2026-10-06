// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "annotation/infrastructure/alignment_review_source.hpp"
#include "annotation/infrastructure/csv_annotation_repository.hpp"
#include "annotation/infrastructure/csv_review_state_store.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace ttrally::annotation;

namespace {

std::filesystem::path temp_dir() {
    std::random_device device;
    auto dir = std::filesystem::temp_directory_path() /
               ("ttrally_annotation_test_" + std::to_string(device()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("label files use the documented format") {
    const auto dir = temp_dir();
    CsvAnnotationRepository repository(dir);
    AnnotationSheet sheet("match_a", {60000, 1001}, 20000);
    sheet.add({12950, 13104, {}, true, ""});
    sheet.add({10234, 10811, 10262, false, ""});
    repository.save(sheet);

    CHECK(read_file(dir / "match_a.csv") ==
          "video_id,rally_id,start_frame,end_frame,fps,serve_contact_frame,flags,notes\n"
          "match_a,1,10234,10811,59.94,10262,,\n"
          "match_a,2,12950,13104,59.94,,aborted_toss,\n");

    const auto loaded = repository.load("match_a");
    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].serve_contact_frame == 10262);
    CHECK(loaded[1].aborted_toss);
    CHECK(repository.load("other_video").empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("frame rates are written with two decimals") {
    CHECK(format_label_fps({60, 1}) == "60");
    CHECK(format_label_fps({60000, 1001}) == "59.94");
    CHECK(format_label_fps({30000, 1001}) == "29.97");
    CHECK(format_label_fps({25, 1}) == "25");
}

TEST_CASE("review items are read from the alignment output") {
    const auto dir = temp_dir();
    std::ofstream(dir / "a.csv")
        << "segment_id,cut_start_s,cut_end_s,orig_start_s,orig_end_s,orig_start_frame,"
           "orig_end_frame,orig_fps,offset_s,confidence\n"
           "1,0.0,5.0,12.0,17.0,719,1020,59.94006,12.0,4.4\n";
    std::ofstream(dir / "a.gaps.csv")
        << "gap_id,after_segment_id,orig_start_s,orig_end_s,orig_start_frame,orig_end_frame,"
           "duration_s\n"
           "1,,0.0,12.0,0,718,12.0\n"
           "2,1,17.0,30.0,1021,1797,13.0\n";
    AlignmentReviewSource source(dir / "a.csv", dir / "a.gaps.csv");
    const auto items = source.load();
    REQUIRE(items.size() == 3);
    CHECK(items[0].kind == ReviewKind::Candidate);
    CHECK(items[0].first_frame == 719);
    CHECK(items[2].source_id == 2);
    CHECK(items[2].last_frame == 1797);
    std::filesystem::remove_all(dir);
}

TEST_CASE("review state round trip") {
    const auto dir = temp_dir();
    CsvReviewStateStore store(dir);
    CHECK(store.load("match_a").empty());
    store.save("match_a", {{{ReviewKind::Gap, 3}, ReviewStatus::Reviewed},
                           {{ReviewKind::Candidate, 7}, ReviewStatus::Rejected}});
    const auto loaded = store.load("match_a");
    REQUIRE(loaded.size() == 2);
    CHECK(loaded.at({ReviewKind::Gap, 3}) == ReviewStatus::Reviewed);
    std::filesystem::remove_all(dir);
}
