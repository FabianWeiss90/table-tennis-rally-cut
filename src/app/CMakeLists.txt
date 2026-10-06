# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

find_package(CLI11 CONFIG REQUIRED)

add_executable(ttrally
    main.cpp)

target_link_libraries(ttrally PRIVATE ttrally_core ttrally_options CLI11::CLI11)
