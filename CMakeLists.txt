# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

cmake_minimum_required(VERSION 3.25)

project(ttrally
    VERSION 0.1.0
    DESCRIPTION "Rally detection for table tennis videos from a hobby camera perspective"
    LANGUAGES CXX)

option(TTRALLY_BUILD_TESTS "Build the unit tests" ON)
option(TTRALLY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TTRALLY_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warning and sanitizer flags shared by all own targets
add_library(ttrally_options INTERFACE)
if(MSVC)
    target_compile_options(ttrally_options INTERFACE /W4 /permissive- /utf-8)
    if(TTRALLY_WARNINGS_AS_ERRORS)
        target_compile_options(ttrally_options INTERFACE /WX)
    endif()
else()
    target_compile_options(ttrally_options INTERFACE -Wall -Wextra -Wpedantic)
    if(TTRALLY_WARNINGS_AS_ERRORS)
        target_compile_options(ttrally_options INTERFACE -Werror)
    endif()
endif()

if(TTRALLY_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "TTRALLY_SANITIZE is only supported with GCC/Clang")
    endif()
    target_compile_options(ttrally_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(ttrally_options INTERFACE -fsanitize=address,undefined)
endif()

add_subdirectory(src)

if(TTRALLY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
