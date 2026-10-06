// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "common/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version string is not empty") { REQUIRE_FALSE(ttrally::version().empty()); }
