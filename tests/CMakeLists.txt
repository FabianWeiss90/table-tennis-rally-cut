# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(ttrally_tests
    test_version.cpp)

target_link_libraries(ttrally_tests PRIVATE ttrally_core ttrally_options Catch2::Catch2WithMain)

catch_discover_tests(ttrally_tests)
