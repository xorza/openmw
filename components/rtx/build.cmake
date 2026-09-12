# The ray tracing renderer's part of the top-level build: how this fork's own code is compiled, where
# its resources and shared shader structures live, and which of its directories are built. Included
# from the top-level `CMakeLists.txt` under `OPENMW_RTX`, so that file carries the options and one line.

# How this fork's own code is compiled, in one place rather than once per target.
#
# Warnings are errors here and nowhere else: the tree around this belongs to upstream and
# `extern/` to third parties, and breaking the build on their warnings would only mean turning
# this off again. The extra warnings were measured before being adopted — each of them costs
# nothing today. Three more were tried and rejected: `-Wfloat-equal` fires 29 times on deliberate
# sentinels, and `-Wold-style-cast` and `-Wuseless-cast` fire inside OpenMW's own headers.
#
# `-Wno-missing-field-initializers` is the one subtraction. Vulkan's create-info structs are
# filled with designated initializers, which value-initialise every field not named — that is the
# point of using them, and GCC does not distinguish it from an accidentally short aggregate.
if (CMAKE_CXX_COMPILER_ID STREQUAL GNU OR CMAKE_CXX_COMPILER_ID MATCHES Clang)
    set(OPENMW_RTX_COMPILE_OPTIONS
        -Werror
        -Wno-missing-field-initializers
        -Wsuggest-override
        -Wzero-as-null-pointer-constant
        -Wnull-dereference
        -Wcast-qual
    )

    # `-Wdouble-promotion` is GCC's only. Clang applies it to implicit argument conversions as
    # well as to arithmetic, which fires nine times inside `components/misc/convert.hpp` — an
    # upstream header whose whole job is handing `osg`'s floats to a double-precision Bullet.
    # The promotion there is the point, the header is not this fork's to change, and the flag
    # still costs nothing on the compiler it was measured on.
    if (CMAKE_CXX_COMPILER_ID STREQUAL GNU)
        list(APPEND OPENMW_RTX_COMPILE_OPTIONS -Wdouble-promotion)
    endif()
endif()

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
