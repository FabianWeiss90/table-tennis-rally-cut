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
    SOURCES media/application/list_decode_backends.cpp
    DEPENDS ttrally_media_domain)

ttrally_layer(ttrally_media_infrastructure
    SOURCES
        media/infrastructure/caching_media_reader.cpp
        media/infrastructure/ffmpeg_api.cpp
        media/infrastructure/ffmpeg_decode_backend_probe.cpp
        media/infrastructure/ffmpeg_frame_sampler.cpp
        media/infrastructure/ffmpeg_logging.cpp
        media/infrastructure/ffmpeg_media_probe.cpp
        media/infrastructure/ffmpeg_media_reader.cpp
    DEPENDS ttrally_media_application ttrally_shared_io
    PRIVATE_DEPENDS ttrally_ffmpeg)

# Command-line interface: composition root
find_package(CLI11 CONFIG REQUIRED)
add_executable(ttrally
    cli/devices_command.cpp
    cli/main.cpp)
target_link_libraries(ttrally PRIVATE
    ttrally_options
    ttrally_media_infrastructure
    CLI11::CLI11)
