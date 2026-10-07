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
    SOURCES media/test_decode_backend.cpp media/test_frame_prefetcher.cpp
    DEPENDS ttrally_media_application)

ttrally_test(annotation_tests
    SOURCES
        annotation/test_annotation_domain.cpp
        annotation/test_annotation_files.cpp
        annotation/test_annotation_session.cpp
    DEPENDS ttrally_annotation_infrastructure)

ttrally_test(features_tests
    SOURCES features/test_extract_features.cpp features/test_features_domain.cpp
    DEPENDS ttrally_features_infrastructure)

ttrally_test(alignment_tests
    SOURCES
        alignment/test_align_videos.cpp
        alignment/test_audio_aligner.cpp
        alignment/test_candidate_mapping.cpp
        alignment/test_signal_matcher.cpp
    DEPENDS ttrally_alignment_infrastructure)

ttrally_test(integration_tests
    SOURCES integration/test_align_media.cpp integration/test_frame_decoder.cpp
    DEPENDS ttrally_alignment_infrastructure ttrally_media_infrastructure)

# Architecture: layer rules, plus a self-test proving that violations are detected
add_test(NAME "architecture: layer dependencies"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/src
            -P ${CMAKE_CURRENT_SOURCE_DIR}/architecture/check_dependencies.cmake)
add_test(NAME "architecture: violations are detected"
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/architecture/fixtures/violation
            -P ${CMAKE_CURRENT_SOURCE_DIR}/architecture/check_dependencies.cmake)
set_tests_properties("architecture: violations are detected" PROPERTIES WILL_FAIL TRUE)

if(TTRALLY_HAS_ORT)
    ttrally_test(onnx_tests
        SOURCES integration/test_onnx_features.cpp
        DEPENDS ttrally_features_onnx ttrally_features_infrastructure ttrally_media_infrastructure)
    target_compile_definitions(onnx_tests
        PRIVATE TTRALLY_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
endif()

if(TTRALLY_BUILD_GUI)
    ttrally_test(gui_tests
        SOURCES gui/test_annotator_smoke.cpp
        DEPENDS ttrally_gui ttrally_media_infrastructure)
endif()
