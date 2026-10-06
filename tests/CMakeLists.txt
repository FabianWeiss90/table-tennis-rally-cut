# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# ttrally_test(<name> SOURCES ... DEPENDS ...)
function(ttrally_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE ${ARG_DEPENDS} ttrally_options Catch2::Catch2WithMain)
    catch_discover_tests(${name} TEST_PREFIX "${name}: ")
endfunction()

ttrally_test(shared_tests
    SOURCES shared/test_frame_timeline.cpp shared/test_io.cpp shared/test_version.cpp
    DEPENDS ttrally_shared_io)

ttrally_test(media_tests
    SOURCES media/test_decode_backend.cpp
    DEPENDS ttrally_media_application)
