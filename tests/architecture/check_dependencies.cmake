# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

# Architecture fitness test: checks the #include directives of all sources against the layer
# rules of the code base (dependencies point inwards only).
#
#   shared/kernel        -> shared/kernel
#   shared/application   -> shared/kernel
#   shared/io            -> shared/kernel
#   <context>/domain     -> shared/kernel, <any context>/domain
#   <context>/application-> the above, shared/application, <any context>/application
#   <context>/infrastructure -> anything except cli; the only layer allowed to use
#                           third-party libraries (FFmpeg, pocketfft, ONNX Runtime) besides cli
#   gui                  -> shared, <any context>/domain and application, gui; may use the GUI
#                           libraries (SDL3, Dear ImGui) but no infrastructure (wired in cli)
#   cli                  -> anything
#
# Usage: cmake -DSOURCE_DIR=<src> -P check_dependencies.cmake

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is not set")
endif()
get_filename_component(SOURCE_DIR "${SOURCE_DIR}" ABSOLUTE)

set(third_party "^(libav|libsw|pocketfft|CLI/|catch2/|SDL3/|imgui|onnxruntime)")

function(allowed_includes file out_project out_third_party)
    if(file MATCHES "^shared/kernel/")
        set(project "^shared/kernel/")
    elseif(file MATCHES "^shared/application/")
        set(project "^shared/(kernel|application)/")
    elseif(file MATCHES "^shared/io/")
        set(project "^shared/(kernel|io)/")
    elseif(file MATCHES "^[a-z_]+/domain/")
        set(project "^(shared/kernel/|[a-z_]+/domain/)")
    elseif(file MATCHES "^[a-z_]+/application/")
        set(project "^(shared/(kernel|application)/|[a-z_]+/(domain|application)/)")
    elseif(file MATCHES "^[a-z_]+/infrastructure/")
        set(project "^(shared/|[a-z_]+/(domain|application|infrastructure)/)")
        set(${out_third_party} TRUE PARENT_SCOPE)
    elseif(file MATCHES "^gui/")
        set(project "^(shared/|[a-z_]+/(domain|application)/|gui/)")
        set(${out_third_party} TRUE PARENT_SCOPE)
    elseif(file MATCHES "^cli/")
        set(project ".*")
        set(${out_third_party} TRUE PARENT_SCOPE)
    else()
        set(project "^$")
    endif()
    set(${out_project} "${project}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE files RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*.hpp" "${SOURCE_DIR}/*.cpp")
set(violations "")
foreach(file IN LISTS files)
    set(third_party_allowed FALSE)
    allowed_includes("${file}" project_pattern third_party_allowed)
    file(STRINGS "${SOURCE_DIR}/${file}" lines REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS lines)
        if(line MATCHES "#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(header "${CMAKE_MATCH_1}")
            if(NOT header MATCHES "${project_pattern}")
                list(APPEND violations "${file}: includes \"${header}\"")
            endif()
        elseif(line MATCHES "#[ \t]*include[ \t]*<([^>]+)>")
            set(header "${CMAKE_MATCH_1}")
            if(header MATCHES "${third_party}" AND NOT third_party_allowed)
                list(APPEND violations "${file}: includes <${header}>")
            endif()
        endif()
    endforeach()
endforeach()

list(LENGTH files file_count)
if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "Layer dependency violations:\n  ${text}")
endif()
message(STATUS "Checked ${file_count} files: no layer dependency violations")
