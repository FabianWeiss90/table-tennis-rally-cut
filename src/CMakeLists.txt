# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

add_library(ttrally_core
    common/version.cpp)

target_include_directories(ttrally_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ttrally_core PRIVATE TTRALLY_VERSION="${PROJECT_VERSION}")
target_link_libraries(ttrally_core PRIVATE ttrally_options)

add_subdirectory(app)
