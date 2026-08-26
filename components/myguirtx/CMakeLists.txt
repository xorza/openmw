# MyGUI over `Rtx::Renderer`, and so over whichever graphics API this build has.
#
# **One backend for both of them.** Most of MyGUI's interface is bookkeeping no API has an opinion
# about; what is left is a table of textures and a list of triangles, which is what `components/rtx`
# offers and each of its backends implements.

set(MYGUIRTX_FILES
    rendermanager.cpp
    rendermanager.hpp
    texture.cpp
    texture.hpp
)

source_group(components\\myguirtx FILES ${MYGUIRTX_FILES})

add_library(openmw-myguirtx STATIC ${MYGUIRTX_FILES})

target_link_libraries(openmw-myguirtx PUBLIC openmw-rtx components)

target_compile_options(openmw-myguirtx PRIVATE ${OPENMW_RTX_COMPILE_OPTIONS})

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-myguirtx PRIVATE --coverage)
    target_link_libraries(openmw-myguirtx gcov)
endif()
