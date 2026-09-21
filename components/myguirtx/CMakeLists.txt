# MyGUI over `Rtx::GuiRenderer`, and so over whichever graphics API this build has.
#
# **One backend for both of them.** Most of MyGUI's interface is bookkeeping no API has an opinion
# about; what is left is a table of textures and a list of triangles, which is what `components/rtx`
# offers and each of its backends implements.

set(RTX_MYGUI_FILES
    additivelayer.cpp
    additivelayer.hpp
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

source_group(components\\myguirtx FILES ${RTX_MYGUI_FILES})

add_library(openmw-rtx-mygui STATIC ${RTX_MYGUI_FILES})

target_link_libraries(openmw-rtx-mygui PUBLIC openmw-rtx components)

openmw_rtx_target(openmw-rtx-mygui)
