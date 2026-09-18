# MyGUI over `Rtx::GuiRenderer`, and so over whichever graphics API this build has.
#
# **One backend for both of them.** Most of MyGUI's interface is bookkeeping no API has an opinion
# about; what is left is a table of textures and a list of triangles, which is what `components/rtx`
# offers and each of its backends implements.

set(MYGUIRTX_FILES
    rendermanager.cpp
    rendermanager.hpp
    rgbarows.cpp
    rgbarows.hpp
    mirrortexture.cpp
    mirrortexture.hpp
    paintedmirror.cpp
    paintedmirror.hpp
    sharedtexture.cpp
    sharedtexture.hpp
    slottexture.cpp
    slottexture.hpp
    texture.cpp
    texture.hpp
)

source_group(components\\myguirtx FILES ${MYGUIRTX_FILES})

add_library(openmw-myguirtx STATIC ${MYGUIRTX_FILES})

target_link_libraries(openmw-myguirtx PUBLIC openmw-rtx components)

openmw_rtx_target(openmw-myguirtx)
