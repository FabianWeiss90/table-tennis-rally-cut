# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

# One library per bounded context and layer. Link dependencies point inwards only:
#   infrastructure -> application -> domain -> shared kernel
# so a forbidden dependency fails at link time. tests/architecture checks the includes.

find_package(Threads REQUIRED)

# pocketfft is header-only; vcpkg installs the header without a CMake package.
find_path(POCKETFFT_INCLUDE_DIR pocketfft_hdronly.h REQUIRED)

# FFmpeg: system packages via pkg-config on Linux, vcpkg on Windows. Linked dynamically.
add_library(ttrally_ffmpeg INTERFACE)
if(WIN32)
    find_package(FFMPEG REQUIRED)
    target_include_directories(ttrally_ffmpeg SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(ttrally_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(ttrally_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    target_link_libraries(ttrally_ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

# ttrally_layer(<name> SOURCES ... DEPENDS ... [PRIVATE_DEPENDS ...])
function(ttrally_layer name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS;PRIVATE_DEPENDS" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PUBLIC ${ARG_DEPENDS} PRIVATE ttrally_options ${ARG_PRIVATE_DEPENDS})
endfunction()

# Shared kernel and shared technical building blocks
ttrally_layer(ttrally_shared_kernel
    SOURCES shared/kernel/frame_timeline.cpp shared/kernel/version.cpp
    DEPENDS Threads::Threads)
target_compile_definitions(ttrally_shared_kernel PRIVATE TTRALLY_VERSION="${PROJECT_VERSION}")

ttrally_layer(ttrally_shared_io
    SOURCES shared/io/csv.cpp shared/io/file_cache.cpp shared/io/formatting.cpp shared/io/npy.cpp
    DEPENDS ttrally_shared_kernel)

# Media context (generic subdomain: reading video files)
ttrally_layer(ttrally_media_domain
    SOURCES media/domain/decode_backend.cpp
    DEPENDS ttrally_shared_kernel)

ttrally_layer(ttrally_media_application
    SOURCES media/application/frame_prefetcher.cpp media/application/list_decode_backends.cpp
    DEPENDS ttrally_media_domain)

ttrally_layer(ttrally_media_infrastructure
    SOURCES
        media/infrastructure/caching_media_reader.cpp
        media/infrastructure/ffmpeg_api.cpp
        media/infrastructure/ffmpeg_decode_backend_probe.cpp
        media/infrastructure/ffmpeg_frame_decoder.cpp
        media/infrastructure/ffmpeg_frame_sampler.cpp
        media/infrastructure/ffmpeg_logging.cpp
        media/infrastructure/ffmpeg_media_probe.cpp
        media/infrastructure/ffmpeg_media_reader.cpp
        media/infrastructure/ffmpeg_video_stream.cpp
    DEPENDS ttrally_media_application ttrally_shared_io
    PRIVATE_DEPENDS ttrally_ffmpeg)

# Alignment context (core domain of phase 1)
ttrally_layer(ttrally_alignment_domain
    SOURCES
        alignment/domain/alignment_settings.cpp
        alignment/domain/audio_aligner.cpp
        alignment/domain/boundary_refinement.cpp
        alignment/domain/candidate_mapping.cpp
        alignment/domain/consistency_check.cpp
        alignment/domain/segmentation.cpp
        alignment/domain/visual_verification.cpp
        alignment/domain/window_search.cpp
    DEPENDS ttrally_shared_kernel ttrally_media_domain)

ttrally_layer(ttrally_alignment_application
    SOURCES alignment/application/align_videos.cpp
    DEPENDS ttrally_alignment_domain ttrally_media_application)

ttrally_layer(ttrally_alignment_infrastructure
    SOURCES
        alignment/infrastructure/csv_alignment_writer.cpp
        alignment/infrastructure/html_report_writer.cpp
        alignment/infrastructure/pocketfft_signal_matcher.cpp
    DEPENDS ttrally_alignment_application ttrally_shared_io)
target_include_directories(ttrally_alignment_infrastructure SYSTEM PRIVATE ${POCKETFFT_INCLUDE_DIR})

# Annotation context (phase 2: exact rally boundaries set by a person)
ttrally_layer(ttrally_annotation_domain
    SOURCES annotation/domain/annotation_sheet.cpp annotation/domain/review_plan.cpp
    DEPENDS ttrally_shared_kernel)

ttrally_layer(ttrally_annotation_application
    SOURCES annotation/application/annotation_session.cpp
    DEPENDS ttrally_annotation_domain)

ttrally_layer(ttrally_annotation_infrastructure
    SOURCES
        annotation/infrastructure/alignment_review_source.cpp
        annotation/infrastructure/csv_annotation_repository.cpp
        annotation/infrastructure/csv_review_state_store.cpp
    DEPENDS ttrally_annotation_application ttrally_shared_io)

# Annotation GUI (presentation layer, Dear ImGui + SDL3)
if(TTRALLY_BUILD_GUI)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    add_library(ttrally_gui STATIC
        gui/annotator_app.cpp
        gui/frame_texture.cpp
        gui/timeline_view.cpp)
    target_include_directories(ttrally_gui PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(ttrally_gui
        PUBLIC ttrally_annotation_application ttrally_media_application
        PRIVATE ttrally_options SDL3::SDL3 imgui::imgui)
endif()

# Command-line interface: composition root
find_package(CLI11 CONFIG REQUIRED)
add_executable(ttrally
    cli/align_command.cpp
    cli/devices_command.cpp
    cli/main.cpp)
target_link_libraries(ttrally PRIVATE
    ttrally_options
    ttrally_alignment_infrastructure
    ttrally_media_infrastructure
    CLI11::CLI11)
if(TTRALLY_BUILD_GUI)
    target_sources(ttrally PRIVATE cli/annotate_command.cpp)
    target_compile_definitions(ttrally PRIVATE TTRALLY_WITH_GUI)
    target_link_libraries(ttrally PRIVATE ttrally_gui ttrally_annotation_infrastructure)
endif()
