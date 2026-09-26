set(OPENMW_SOURCES
    engine.cpp
    options.cpp
)

set(OPENMW_RESOURCES
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.rc
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.exe.manifest
)

set(OPENMW_HEADERS
    doc.hpp
    engine.hpp
    options.hpp
    profile.hpp
)

source_group(apps/openmw FILES main.cpp androidmain.cpp ${OPENMW_SOURCES} ${OPENMW_HEADERS} ${OPENMW_RESOURCES})

# The renderer seam: `MWRender::Renderer`, what a frame is described as, and the rasterizer behind
# the seam. Built whichever renderer is, and named once so the block below can find them.
set(OPENMW_RENDERER_SEAM
    renderer sceneframe framedescriber skystate ripplerules objectstorage precipitation ground glground glrenderer glworld gloffscreenview glmapoverlay mapoverlay pixels
    )

add_openmw_dir (mwrender
    actors objects renderingmanager animation rotatecontroller sky skyutil npcanimation esm4npcanimation vismask
    creatureanimation effectmanager util renderinginterface pathgrid rendermode weaponanimation screenshotmanager
    bulletdebugdraw globalmap characterpreview camera localmap water terrainstorage ripplesimulation
    renderbin actoranimation landmanager navmesh actorspaths recastmesh fogmanager objectpaging groundcover
    postprocessor pingpongcull luminancecalculator pingpongcanvas transparentpass opaqueblit waterawaretransparentbin precipitationocclusion ripples
    actorutil distortion animationpriority bonegroup blendmask animblendcontroller
    ${OPENMW_RENDERER_SEAM}
    )

# Gated, because these are the only files in the game that name a graphics API other than OpenGL.
if (OPENMW_RTX)
    set(OPENMW_RTX_DIR
        classmasks debugwalk framereport frametimer rippleemitters rtxrenderer rtxrun rtxsettings rtxwindow skyreader tracedground
        tracedoverlay tracedterrain tracedview viewqueue worldmirror
        )
    add_openmw_dir (mwrender/rtx ${OPENMW_RTX_DIR})

    # The seam and the ray tracer's own directory are this fork's files in upstream's target, and
    # every one of them reads the game's headers: the errors alone, for the reason `build.cmake`
    # gives. A name with a header alone names a file nothing compiles, and a property on it is
    # read by nothing.
    set(OPENMW_RTX_SOURCES)
    set(OPENMW_RTX_NAMED)
    foreach (name ${OPENMW_RTX_DIR})
        list(APPEND OPENMW_RTX_SOURCES mwrender/rtx/${name}.cpp)
        list(APPEND OPENMW_RTX_NAMED ${name}.cpp ${name}.hpp)
    endforeach()
    openmw_rtx_expect_listed("${CMAKE_CURRENT_SOURCE_DIR}/mwrender/rtx" MATCHING *.cpp *.hpp LISTED ${OPENMW_RTX_NAMED})
    foreach (name ${OPENMW_RENDERER_SEAM})
        list(APPEND OPENMW_RTX_SOURCES mwrender/${name}.cpp)
    endforeach()
    openmw_rtx_sources(ERRORS_ONLY ${OPENMW_RTX_SOURCES})
endif()

add_openmw_dir (mwinput
    actions actionmanager bindingsmanager controllermanager controlswitch
    inputmanagerimp mousemanager keyboardmanager sensormanager gyromanager
    )

add_openmw_dir (mwgui
    layout textinput widgets race class birth review windowmanagerimp console dialogue
    windowbase statswindow messagebox journalwindow charactercreation
    mapwindow windowpinnablebase tooltips scrollwindow bookwindow resourceskin
    formatting inventorywindow container hud countdialog tradewindow settingswindow
    confirmationdialog alchemywindow referenceinterface spellwindow mainmenu quickkeysmenu
    itemselection spellbuyingwindow loadingscreen levelupdialog waitdialog spellcreationdialog
    enchantingdialog trainingwindow travelwindow exposedwindow cursor spellicons
    merchantrepair repair soulgemdialog companionwindow bookpage journalviewmodel journalbooks
    itemmodel containeritemmodel inventoryitemmodel sortfilteritemmodel itemview
    tradeitemmodel companionitemmodel pickpocketitemmodel controllers savegamedialog
    recharge mode videowidget backgroundimage itemwidget screenfader debugwindow spellmodel spellview
    draganddrop timeadvancer jailscreen itemchargeview keyboardnavigation textcolours statswatcher
    postprocessorhud settings worlditemmodel itemtransfer controllerbuttonsoverlay inventorytabsoverlay
    )

add_openmw_dir (mwdialogue
    dialoguemanagerimp journalimp journalentry quest topic filter selectwrapper keywordsearch scripttest
    )

add_openmw_dir (mwscript
    locals scriptmanagerimp compilercontext interpretercontext cellextensions miscextensions
    guiextensions soundextensions skyextensions statsextensions containerextensions
    aiextensions controlextensions extensions globalscripts ref dialogueextensions
    animationextensions transformationextensions consoleextensions userextensions
    )

add_openmw_dir (mwlua
    objectcache luamanagerimp object objectlists userdataserializer luaevents engineevents objectvariant loadscripts contentbindings
    context menuscripts globalscripts localscripts playerscripts luabindings objectbindings cellbindings coremwscriptbindings
    mwscriptbindings camerabindings vfsbindings uibindings soundbindings inputbindings nearbybindings dialoguebindings
    postprocessingbindings stats recordstore debugbindings corebindings worldbindings worker landbindings magicbindings factionbindings
    classbindings itemdata inputprocessor animationbindings birthsignbindings racebindings markupbindings weatherbindings regionbindings
    types/types types/door types/item types/actor types/container types/lockable types/weapon types/npc types/bodypart
    types/creature types/player types/activator types/book types/lockpick types/probe types/apparatus
    types/potion types/ingredient types/misc types/repair types/armor types/light types/static
    types/clothing types/levelledlist types/terminal magictypebindings
    )

add_openmw_dir (mwsound
    soundmanagerimp openaloutput ffmpegdecoder headcache warmqueue sound soundbuffer sounddecoder soundoutput
    loudness movieaudiofactory alext efx efxpresets regionsoundselector watersoundupdater
    )

add_openmw_dir (mwworld
    refdata worldimp scene globals class action nullaction actionteleport
    containerstore actiontalk actiontake manualref player cellvisitors failedaction
    worldmodel localscripts customdata inventorystore ptr actionopen actionread actionharvest
    actionequip timestamp actionalchemy cellstore actionapply actioneat
    store esmstore fallback actionrepair actionsoulgem livecellref actiondoor
    contentloader esmloader actiontrap cellreflist cellref weather projectilemanager
    cellpreloader datetimemanager groundcoverstore magiceffects cell ptrregistry
    positioncellgrid
    )

add_openmw_dir (mwphysics
    physicssystem trace collisiontype actor convert object heightfield closestnotmerayresultcallback
    contacttestresultcallback stepper movementsolver projectile
    actorconvexcallback raycasting mtphysics contacttestwrapper projectileconvexcallback
    )

add_openmw_dir (mwclass
    classes activator creature npc weapon armor potion apparatus book clothing container door
    ingredient creaturelevlist itemlevlist light lockpick misc probe repair static actor bodypart
    esm4base esm4npc light4
    )

add_openmw_dir (mwmechanics
    mechanicsmanagerimp stat creaturestats magiceffects movement actorutil spelllist
    drawstate spells activespells npcstats aipackage aisequence aipursue alchemy aiwander aitravel aifollow aiavoiddoor aibreathe
    aicast aiescort aiface aiactivate aicombat recharge repair enchanting pathfinding pathgrid security spellcasting spellresistance
    disease pickpocket levelledlist combat steering obstacle autocalcspell difficultyscaling aicombataction summoning
    character actors objects aistate weaponpriority spellpriority weapontype spellutil
    spelleffects
    )

add_openmw_dir (mwstate
    statemanagerimp charactermanager character quicksavemanager
    )

add_openmw_dir (mwbase
    environment world scriptmanager dialoguemanager journal soundmanager mechanicsmanager
    inputmanager windowmanager statemanager luamanager
    )

# Main executable

add_library(openmw-lib STATIC
    ${OPENMW_FILES}
    ${OPENMW_SOURCES}
)

if (MSVC)
    # these TUs take a lot of memory for codegen
    # in CI, they often go over the 8GB memory limit if another cl.exe is running at the same time
    # this option (make sure you're not looking at the docs for the identically-named linker option) stops four codegen threads being spun off
    # it hurts build time ~33% to enable this unnecessarily, though, so target it to the hungriest boys
    set_source_files_properties(
        mwlua/dialoguebindings.cpp
        mwlua/magicbindings.cpp
        mwlua/objectbindings
        mwlua/stats.cpp
        mwworld/esmstore.cpp
        PROPERTIES
        COMPILE_OPTIONS /cgthreads1
    )
endif()

if(BUILD_OPENMW)
    if (ANDROID)
        add_library(openmw SHARED
            main.cpp
            androidmain.cpp
        )
    else()
        openmw_add_executable(openmw
            ${APPLE_BUNDLE_RESOURCES}
            ${OPENMW_RESOURCES}
            main.cpp
        )
    endif()

    target_link_libraries(openmw openmw-lib)

if (OPENMW_RTX)
    target_link_libraries(openmw-lib openmw-rtx openmw-rtx-vulkan openmw-rtx-bench openmw-rtx-mygui)
endif()

    # Workaround necessary to ensure osgAnimation::MatrixLinearSampler dynamic casts work under Clang
    # NOTE: it's unclear whether the broken behavior is spec-compliant
    if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        set_target_properties(openmw PROPERTIES ENABLE_EXPORTS ON)
    endif()
endif()

# Sound stuff - here so CMake doesn't stupidly recompile EVERYTHING
# when we change the backend.
include_directories(
    ${FFmpeg_INCLUDE_DIRS}
)

target_link_libraries(openmw-lib
    # CMake's built-in OSG finder does not use pkgconfig, so we have to
    # manually ensure the order is correct for inter-library dependencies.
    # This only makes a difference with `-DOPENMW_USE_SYSTEM_OSG=ON -DOSG_STATIC=ON`.
    # https://gitlab.kitware.com/cmake/cmake/-/issues/21701
    ${OSGPARTICLE_LIBRARIES}
    ${OSGVIEWER_LIBRARIES}
    ${OSGGA_LIBRARIES}
    ${OSGSHADOW_LIBRARIES}
    ${OSGDB_LIBRARIES}
    ${OSGUTIL_LIBRARIES}
    ${OSG_LIBRARIES}
    Boost::program_options
    ${OPENAL_LIBRARY}
    ${FFmpeg_LIBRARIES}
    ${MyGUI_LIBRARIES}
    SDL2::SDL2
    ${RecastNavigation_LIBRARIES}
    osg-ffmpeg-videoplayer
    oics
    components
)

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-lib PRIVATE
        <boost/program_options/options_description.hpp>

        <sol/sol.hpp>

        <osg/StateSet>
        <osg/Node>
        <osg/Drawable>
        <osg/Camera>

        <osgViewer/Viewer>

        <MyGUI_Widget.h>

        <algorithm>
        <filesystem>
        <fstream>
        <functional>
        <memory>
        <string>
        <string_view>
        <vector>
    )
endif()

if (ANDROID)
    target_link_libraries(openmw-lib EGL android log z)
endif (ANDROID)

if (USE_SYSTEM_TINYXML)
    target_link_libraries(openmw-lib ${TinyXML_LIBRARIES})
endif()

if (NOT UNIX)
    target_link_libraries(openmw-lib ${SDL2MAIN_LIBRARY})
endif()

# Fix for not visible pthreads functions for linker with glibc 2.15
if (UNIX AND NOT APPLE)
    target_link_libraries(openmw-lib ${CMAKE_THREAD_LIBS_INIT})
endif()

if(APPLE AND BUILD_OPENMW)
    set(BUNDLE_RESOURCES_DIR "${APP_BUNDLE_DIR}/Contents/Resources")

    set(OPENMW_RESOURCES_ROOT ${BUNDLE_RESOURCES_DIR})

    add_subdirectory(../../files/ ${CMAKE_CURRENT_BINARY_DIR}/files)

    configure_file("${OpenMW_BINARY_DIR}/defaults.bin" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/openmw.cfg" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/gamecontrollerdb.txt" ${BUNDLE_RESOURCES_DIR} COPYONLY)

    add_custom_command(TARGET openmw
        POST_BUILD
        COMMAND cp "${OpenMW_BINARY_DIR}/resources/version" "${BUNDLE_RESOURCES_DIR}/resources")

    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    target_link_libraries(openmw ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK})

    if (FFmpeg_FOUND)
        target_link_options(openmw PRIVATE "LINKER:SHELL:-framework CoreVideo"
                                           "LINKER:SHELL:-framework CoreMedia"
                                           "LINKER:SHELL:-framework VideoToolbox"
                                           "LINKER:SHELL:-framework AudioToolbox"
                                           "LINKER:SHELL:-framework VideoDecodeAcceleration")
    endif()
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-lib PRIVATE --coverage)
    target_link_libraries(openmw-lib gcov)
    if (NOT ANDROID AND BUILD_OPENMW)
        target_compile_options(openmw PRIVATE --coverage)
        target_link_libraries(openmw gcov)
    endif()
endif()

if (NOT APPLE AND BUILD_OPENMW)
    INSTALL(TARGETS openmw)
endif()
