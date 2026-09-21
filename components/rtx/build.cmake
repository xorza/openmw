# The ray tracing renderer's part of the top-level build: how this fork's own code is compiled, where
# its resources and shared shader structures live, and which of its directories are built. Included
# from the top-level `CMakeLists.txt` under `OPENMW_RTX`, so that file carries the options and one
# line — before `components`, so its CMakeLists can name the fork's files there.

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
# and CMake refuses a target the two forms are mixed on.
function(openmw_rtx_target target)
    target_compile_options(${target} PRIVATE ${OPENMW_RTX_ERRORS} ${OPENMW_RTX_CHECKS})
    if (BUILD_WITH_CODE_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage)
        target_link_libraries(${target} PRIVATE gcov)
    endif()
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

# Where the RTX resources land. Not `OPENMW_RESOURCES_ROOT`: the top level only defines that for
# non-Apple builds, and macOS derives its own — the bundle's `Contents/Resources` — inside
# `apps/openmw/CMakeLists.txt`, which is configured after these subdirectories. Both roots it
# would resolve to are known here, so this settles it once instead of depending on the order two
# subdirectories happen to be added in.
if (APPLE)
    set(RTX_RESOURCES_ROOT "${APP_BUNDLE_DIR}/Contents/Resources")
else()
    set(RTX_RESOURCES_ROOT "${OpenMW_BINARY_DIR}")
endif()

# Where the structures shared with every shader language live. Both backends compile against
# them, so the path is settled once rather than in each.
set(RTX_SHADER_INCLUDE "${OpenMW_SOURCE_DIR}/components/rtx/shaders")

add_subdirectory (components/rtx)
add_subdirectory (components/rtxbench)

add_subdirectory (components/rtxvulkan)
add_subdirectory (components/myguirtx)

# The harness drives a real game, so there has to be one to drive. Its library is still built
# without one, because `components-tests` reaches into it and needs no engine.
add_subdirectory (apps/rtxtool)
