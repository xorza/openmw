# The ray tracing renderer's part of the top-level build: how this fork's own code is compiled, where
# its resources and shared shader structures live, and which of its directories are built. Included
# from the top-level `CMakeLists.txt` under `OPENMW_RTX`, so that file carries the options and one
# line — before `components`, so its CMakeLists can name the fork's files there.

# Vulkan ray tracing on NVIDIA hardware, which macOS has neither of. Refused by name rather than
# left to fail at the first Vulkan header, and off there unless asked for.
if (APPLE)
    message(FATAL_ERROR "The ray tracer needs Vulkan on NVIDIA hardware, which macOS has not: "
                        "configure with -DOPENMW_RTX=OFF")
endif()

# **CMake 3.31, for the presets.** `CMakePresets.json` is where a build of this fork is described —
# `apps/rtxtool/rtx` configures through it and so does an IDE — and a presets file of that version
# is the first that can carry a `$comment` beside each decision, which is how everything in this
# tree is written. Upstream's own floor stays where it is: this is asked only of a ray-tracing build.
if (CMAKE_VERSION VERSION_LESS 3.31)
    message(FATAL_ERROR "The ray tracer's build needs CMake 3.31 (this is ${CMAKE_VERSION}): "
                        "CMakePresets.json is written at version 10, for its comments")
endif()

# How this fork's own code is compiled, in one place rather than once per target.
#
# Warnings are errors here and nowhere else: the tree around this belongs to upstream and
# `extern/` to third parties, and breaking the build on their warnings would only mean turning
# this off again. The extra checks were measured before being adopted — each of them costs
# nothing today. Three more were tried and rejected: `-Wfloat-equal` fires 29 times on deliberate
# sentinels, and `-Wold-style-cast` and `-Wuseless-cast` fire inside OpenMW's own headers.
#
# `-Wno-missing-field-initializers` is the one subtraction. Vulkan's create-info structs are
# filled with designated initializers, which value-initialise every field not named — that is the
# point of using them, and GCC does not distinguish it from an accidentally short aggregate.
#
# **Two lists, because some of this fork's files read the game's own headers.** Those headers are
# not clean under the extra checks — `-Wsuggest-override` and `-Wzero-as-null-pointer-constant`
# fire inside them — so a file that includes one cannot take the checks. What it can take is the
# posture: `OPENMW_RTX_ERRORS` is the errors and the subtraction, for every file of this fork's
# wherever it is built; `OPENMW_RTX_CHECKS` is the extra checks, for every file free of those
# headers. Left on upstream's flags, a file that reads them warned about its designated
# initializers and nobody saw it, since nothing there is an error. The two functions below are
# how a target and a file take them, and a file that takes the errors alone is named where its
# target is, with the reason.
#
# Told apart by the command line a compiler takes, which is what the flags are about: clang-cl
# reports itself as Clang and takes MSVC's, and `MSVC` is set for it as well.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL MSVC OR MSVC)
    # The same posture under the other command line: upstream's `/W4` and its four exclusions
    # apply to the whole tree, and here they are errors. Two more come off, C4244 and C4267 —
    # `/W4`'s implicit narrowing between arithmetic types, which is `-Wconversion`, a check GCC's
    # `-Wall -Wextra` does not make and this tree never adopted; forty test lines narrow a
    # `size_t` to the `int` an API takes, and the bar is the one the code was written to. No extra
    # checks under this command line.
    set(OPENMW_RTX_ERRORS /WX /wd4244 /wd4267)
    set(OPENMW_RTX_CHECKS)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL GNU OR CMAKE_CXX_COMPILER_ID MATCHES Clang)
    set(OPENMW_RTX_ERRORS -Werror -Wno-missing-field-initializers)
    set(OPENMW_RTX_CHECKS -Wsuggest-override -Wzero-as-null-pointer-constant -Wnull-dereference -Wcast-qual)

    # Two checks GCC 13 gets wrong, measured on Ubuntu 24.04's compiler and taken off there:
    # `-Wmaybe-uninitialized` reports the payload of a `std::optional` on every copy of a struct
    # that holds one (GCC bug 80635; `materialresolver.cpp`), and `-Wnull-dereference` reports
    # `std::construct_at` inside `std::vector<osg::Node*>::insert`, reached through OSG's own
    # inline `pushOntoNodePath` (the tests). GCC 15 compiles the whole tree clean with both on, so
    # each check is kept where it is right. The first is upstream's `-Wall` and so reaches every
    # file; the second is one of the extra checks.
    if (CMAKE_CXX_COMPILER_ID STREQUAL GNU AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        list(APPEND OPENMW_RTX_ERRORS -Wno-maybe-uninitialized)
        list(APPEND OPENMW_RTX_CHECKS -Wno-null-dereference)
    endif()

    # One check GCC 16 gets wrong, measured on Arch's 16.2 and taken off there:
    # `-Wstringop-overflow` reports growing a `std::vector` of bytes with `resize(n, value)` as eight
    # bytes written into an allocation of one to five, where the growth is inlined into a test at
    # `-O3` and the fill's move of the old bytes is vectorised. The path is one that cannot happen,
    # seen by a pass that runs before the sizes are folded — the class of GCC bugs 107852, 117983 and
    # 118521 — and it came back at a second call site after the first was written around, so it is
    # the check that is taken off and not the call site that is bent. It is on by default and so
    # reaches every file.
    if (CMAKE_CXX_COMPILER_ID STREQUAL GNU AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
        list(APPEND OPENMW_RTX_ERRORS -Wno-stringop-overflow)
    endif()

    # `-Wdouble-promotion` is GCC's only. Clang applies it to implicit argument conversions as
    # well as to arithmetic, which fires nine times inside `components/misc/convert.hpp` — an
    # upstream header whose whole job is handing `osg`'s floats to a double-precision Bullet.
    # The promotion there is the point, the header is not this fork's to change, and the flag
    # still costs nothing on the compiler it was measured on.
    if (CMAKE_CXX_COMPILER_ID STREQUAL GNU)
        list(APPEND OPENMW_RTX_CHECKS -Wdouble-promotion)
    endif()
endif()

# A target of this fork's own: the errors, the checks, and coverage where the build asks for it.
# The keyword form of `target_link_libraries`, because every one of these targets links with it,
# and CMake refuses a target the two forms are mixed on. Each is also listed for `openmw-rtx-all`.
function(openmw_rtx_target target)
    set_property(GLOBAL APPEND PROPERTY OPENMW_RTX_TARGETS ${target})
    target_compile_options(${target} PRIVATE ${OPENMW_RTX_ERRORS} ${OPENMW_RTX_CHECKS})
    if (BUILD_WITH_CODE_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage)
        target_link_libraries(${target} PRIVATE gcov)
    endif()
endfunction()

# A library of this fork's own, out of the files named: grouped for an IDE under its directory
# the way upstream groups its own, static like every library in this tree, and compiled with the
# flags above. What each library links, and why publicly, stays in its own file.
function(openmw_rtx_library target)
    file(RELATIVE_PATH group "${OpenMW_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    string(REPLACE "/" "\\" group "${group}")
    source_group("${group}" FILES ${ARGN})
    add_library(${target} STATIC ${ARGN})
    openmw_rtx_target(${target})
endfunction()

# This fork's files inside a target that is not its own — the game, upstream's test binaries —
# where every other file keeps upstream's flags. The files named after `ERRORS_ONLY` read the
# game's headers and take the errors without the checks; a file named on both sides is one of
# those, so a caller can hand over a whole list and then name its exceptions.
function(openmw_rtx_sources)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "ERRORS_ONLY")
    set(strict ${arg_UNPARSED_ARGUMENTS})
    if (strict AND arg_ERRORS_ONLY)
        list(REMOVE_ITEM strict ${arg_ERRORS_ONLY})
    endif()
    # Joined as a list and not as a string, so an empty check list adds no empty option.
    set(options ${OPENMW_RTX_ERRORS} ${OPENMW_RTX_CHECKS})
    if (strict)
        set_source_files_properties(${strict} PROPERTIES COMPILE_OPTIONS "${options}")
    endif()
    if (arg_ERRORS_ONLY)
        set_source_files_properties(${arg_ERRORS_ONLY} PROPERTIES COMPILE_OPTIONS "${OPENMW_RTX_ERRORS}")
    endif()
endfunction()

# Stops the configure where a file under `directory` that a `MATCHING` pattern finds is not in
# `LISTED`: the whole of what the directory's lists name, conditional ones included. **The lists are
# written by hand and nothing but this reads them against the tree**: a source left out is never
# compiled, a shader left out is never built, and a header left out is missing from every IDE's
# view of its target — and none of the three says so. The patterns are relative to `directory` and
# reach into its subdirectories; a listed path is relative to it or absolute. `CONFIGURE_DEPENDS`,
# so a file added later reruns this at the next build rather than at the next configure somebody
# remembers to run.
function(openmw_rtx_expect_listed directory)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "MATCHING;LISTED")
    set(patterns)
    foreach (pattern ${arg_MATCHING})
        list(APPEND patterns "${directory}/${pattern}")
    endforeach()
    file(GLOB_RECURSE unlisted CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE "${directory}" ${patterns})

    foreach (listed ${arg_LISTED})
        if (IS_ABSOLUTE "${listed}")
            file(RELATIVE_PATH listed "${directory}" "${listed}")
        endif()
        list(REMOVE_ITEM unlisted "${listed}")
    endforeach()

    if (unlisted)
        list(JOIN unlisted ", " named)
        message(FATAL_ERROR "${directory}: the lists name every file here but ${named}")
    endif()
endfunction()

# **Vulkan, found once and at the top.** An imported target is scoped to the directory that finds
# it, and two directories link this one: the backend, and the tests that reach into the backend's
# own headers, which a host never does — the backend keeps Vulkan private behind
# `createrenderer.hpp`. Found here, both see the same target.
#
# **1.4.333 at least**, the headers that first name everything the backend requires: the hit objects
# of `VK_EXT_ray_tracing_invocation_reorder` came at 1.4.333, and `VK_KHR_shader_fma`, which every
# shader's fusions are (`Rtx::pinFloatArithmetic`), at 1.4.329. An older SDK is a configure that
# says so rather than a compile that stops inside `requirements.cpp`.
find_package(Vulkan 1.4.333 REQUIRED)

# Where the compiled shaders land, beside the other RTX resources: the backend writes them, the
# game and the harness read them through `resources/`, and the tests are told the path outright.
#
# **Two sets, because the driver keys its cache on the bytes it is handed.** Every renderer reads
# the first, which holds no source: with the source in it, an edited comment was a module the
# driver had never seen, compiled again, and profiled and replaced again over the next processes
# (`RtxTool::DriverCache`). The second is the same modules with their source, for a profiler that
# shows a shader's lines — the harness's `--shader-source`.
set(RTX_SPIRV_DIR "${OPENMW_RESOURCES_ROOT}/resources/rtx/shaders")
set(RTX_SPIRV_SOURCE_DIR "${OPENMW_RESOURCES_ROOT}/resources/rtx/shaders-source")

# Where the structures shared with every shader language live. Both backends compile against
# them, so the path is settled once rather than in each.
set(RTX_SHADER_INCLUDE "${OpenMW_SOURCE_DIR}/components/rtx/shaders")

add_subdirectory (components/rtx)
add_subdirectory (components/rtxvulkan)
add_subdirectory (components/myguirtx)

# The harness drives a real game, so there has to be one to drive. Its library is still built
# without one, because `components-tests` reaches into it and needs no engine.
add_subdirectory (apps/rtxtool)

# **Every target of this fork's own, by one name**: what `openmw_rtx_target` was handed, so a
# target added later is in it without anybody listing it. What `rtx` compiles without asserts.
add_custom_target(openmw-rtx-all)
get_property(_openmw_rtx_targets GLOBAL PROPERTY OPENMW_RTX_TARGETS)
add_dependencies(openmw-rtx-all ${_openmw_rtx_targets})
unset(_openmw_rtx_targets)
