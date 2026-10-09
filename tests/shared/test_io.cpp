// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "shared/io/csv.hpp"
#include "shared/io/file_cache.hpp"
#include "shared/io/formatting.hpp"
#include "shared/io/json.hpp"
#include "shared/io/npy.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE("NPY float32 2-D round trip") {
    const auto path = temp_file("ttrally_test_2d.npy");
    const std::vector<float> data{1.0F, 2.5F, -3.0F, 4.0F, 5.0F, 6.25F};
    const std::array<std::size_t, 2> shape{2, 3};
    ttrally::io::write_npy<float>(path, data, shape);

    // Data must start at a multiple of 64 bytes
    CHECK(std::filesystem::file_size(path) % 64 == data.size() * sizeof(float) % 64);

    const auto array = ttrally::io::read_npy<float>(path);
    CHECK(array.shape == std::vector<std::size_t>{2, 3});
    CHECK(array.data == data);
    std::filesystem::remove(path);
}

TEST_CASE("NPY int64 1-D round trip and type check") {
    const auto path = temp_file("ttrally_test_1d.npy");
    const std::vector<std::int64_t> data{0, 1001, 2002, -5};
    const std::array<std::size_t, 1> shape{4};
    ttrally::io::write_npy<std::int64_t>(path, data, shape);

    const auto array = ttrally::io::read_npy<std::int64_t>(path);
    CHECK(array.shape == std::vector<std::size_t>{4});
    CHECK(array.data == data);
    CHECK_THROWS(ttrally::io::read_npy<float>(path));
    std::filesystem::remove(path);
}

TEST_CASE("NPY header uses Python tuple syntax") {
    const auto path = temp_file("ttrally_test_header.npy");
    const std::vector<double> data{1.0, 2.0, 3.0};
    const std::array<std::size_t, 1> shape{3};
    ttrally::io::write_npy<double>(path, data, shape);
    std::ifstream in(path, std::ios::binary);
    std::string header(128, '\0');
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    CHECK(header.find("'shape': (3,)") != std::string::npos);
    CHECK(header.find("'descr': '<f8'") != std::string::npos);
    in.close();
    std::filesystem::remove(path);
}

TEST_CASE("CSV escaping and round trip") {
    CHECK(ttrally::io::csv_escape("plain") == "plain");
    CHECK(ttrally::io::csv_escape("a,b") == "\"a,b\"");
    CHECK(ttrally::io::csv_escape("say \"hi\"") == "\"say \"\"hi\"\"\"");

    const auto path = temp_file("ttrally_test.csv");
    ttrally::io::write_csv(path, {"id", "note"}, {{"1", "a,b"}, {"2", ""}});
    const auto rows = ttrally::io::read_csv(path);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == ttrally::io::CsvRow{"id", "note"});
    CHECK(rows[1] == ttrally::io::CsvRow{"1", "a,b"});
    CHECK(rows[2] == ttrally::io::CsvRow{"2", ""});
    std::filesystem::remove(path);
}

TEST_CASE("frame rate formatting") {
    CHECK(ttrally::io::format_fps({60, 1}) == "60");
    CHECK(ttrally::io::format_fps({60000, 1001}) == "59.94006");
    CHECK(ttrally::io::format_fps({25, 1}) == "25");
    CHECK(ttrally::io::format_fps({30000, 1001}) == "29.97003");
}

TEST_CASE("clock formatting") {
    CHECK(ttrally::io::format_clock(0.0) == "0:00:00.000");
    CHECK(ttrally::io::format_clock(3723.4567) == "1:02:03.457");
    CHECK(ttrally::io::format_clock(-1.5) == "-0:00:01.500");
}

TEST_CASE("cache keys depend on purpose and file") {
    const auto path = std::filesystem::temp_directory_path() / "ttrally_test_cache_key.bin";
    std::ofstream(path) << "content";
    const auto a = ttrally::io::cache_key(path, "audio");
    CHECK(a.size() == 16);
    CHECK(a == ttrally::io::cache_key(path, "audio"));
    CHECK(a != ttrally::io::cache_key(path, "video"));
    std::filesystem::remove(path);
}

TEST_CASE("JSON documents of manifests and model metadata are parsed") {
    using ttrally::io::JsonError;
    using ttrally::io::parse_json;
    const auto value = parse_json(R"json({
        "model": "facebook/dinov2-base (fp16)", "rows": 4928, "rate": 10.0,
        "parts": ["cls", "mean"], "input_size": [392, 224], "fp16": true, "note": null,
        "quoted": "a \"b\"\nc"
    })json");
    CHECK(value.at("model").as_string() == "facebook/dinov2-base (fp16)");
    CHECK(value.at("rows").as_number() == 4928.0);
    CHECK(value.at("parts").as_array().size() == 2);
    CHECK(value.at("input_size").as_array()[1].as_number() == 224.0);
    CHECK(value.at("fp16").as_bool());
    CHECK(value.at("note").is_null());
    CHECK(value.at("quoted").as_string() == "a \"b\"\nc");
    CHECK_FALSE(value.contains("missing"));
    CHECK_THROWS_AS(value.at("missing"), JsonError);
    CHECK_THROWS_AS(value.at("rows").as_string(), JsonError);
    CHECK(parse_json("[]").as_array().empty());
    CHECK(parse_json("-1.5e2").as_number() == -150.0);
    for (const char* broken : {"", "{", "[1,]", "{\"a\" 1}", "\"open", "1 2", "nul"}) {
        CHECK_THROWS_AS(parse_json(broken), JsonError);
    }
}

