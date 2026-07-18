set(OPENMW_SOURCES
    engine.cpp
    fnvsidecaripc.cpp
    options.cpp
)

set(OPENMW_RESOURCES
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.rc
    ${CMAKE_SOURCE_DIR}/files/windows/openmw.exe.manifest
)

set(OPENMW_HEADERS
    doc.hpp
    engine.hpp
    fnvsidecaripc.hpp
    options.hpp
    profile.hpp
)

source_group(apps/openmw FILES main.cpp androidmain.cpp ${OPENMW_SOURCES} ${OPENMW_HEADERS} ${OPENMW_RESOURCES})

add_openmw_dir (mwrender
    actors objects renderingmanager animation rotatecontroller sky skyutil npcanimation esm4npcanimation vismask
    creatureanimation effectmanager util renderinginterface pathgrid rendermode weaponanimation screenshotmanager
    bulletdebugdraw globalmap characterpreview camera localmap water terrainstorage ripplesimulation
    renderbin actoranimation landmanager navmesh actorspaths recastmesh fogmanager objectpaging groundcover
    postprocessor pingpongcull luminancecalculator pingpongcanvas transparentpass precipitationocclusion ripples
    actorutil distortion animationpriority bonegroup blendmask animblendcontroller
    )

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
    dialoguemanagerimp journalimp journalentry quest topic filter selectwrapper hypertextparser keywordsearch scripttest
    )

add_openmw_dir (mwscript
    locals scriptmanagerimp compilercontext interpretercontext cellextensions miscextensions
    guiextensions soundextensions skyextensions statsextensions containerextensions
    aiextensions controlextensions extensions globalscripts ref dialogueextensions
    animationextensions transformationextensions consoleextensions userextensions
    )

add_openmw_dir (mwlua
    luamanagerimp object objectlists userdataserializer luaevents engineevents objectvariant
    context menuscripts globalscripts localscripts playerscripts luabindings objectbindings cellbindings coremwscriptbindings
    mwscriptbindings camerabindings vfsbindings uibindings soundbindings inputbindings nearbybindings dialoguebindings
    postprocessingbindings stats recordstore debugbindings corebindings worldbindings worker landbindings magicbindings factionbindings
    classbindings itemdata inputprocessor animationbindings birthsignbindings racebindings markupbindings weatherbindings regionbindings
    vrbindings
    types/types types/door types/item types/actor types/container types/lockable types/weapon types/npc
    types/creature types/player types/activator types/book types/lockpick types/probe types/apparatus
    types/potion types/ingredient types/misc types/repair types/armor types/light types/static
    types/clothing types/levelledlist types/terminal
    )

add_openmw_dir (mwsound
    soundmanagerimp openaloutput ffmpegdecoder sound soundbuffer sounddecoder soundoutput
    loudness movieaudiofactory alext efx efxpresets regionsoundselector saysequence watersoundupdater
    )

add_openmw_dir (mwworld
    refdata worldimp scene globals class action nullaction actionteleport
    containerstore actiontalk actiontake manualref player cellvisitors failedaction
    worldmodel localscripts customdata inventorystore ptr actionopen actionread actionharvest actionesm4radio actionesm4terminal
    actionequip timestamp actionalchemy cellstore actionapply actioneat
    store esmstore fallback actionrepair actionsoulgem livecellref actiondoor
    esm4questruntime fnvplayerstate fnvplayerruntimestate fnvterminalruntime
    contentloader esmloader actiontrap cellreflist cellref weather projectilemanager
    cellpreloader datetimemanager groundcoverstore magiceffects cell ptrregistry
    positioncellgrid
    )

add_openmw_dir (mwphysics
    physicssystem trace collisiontype actor convert object heightfield closestnotmerayresultcallback
    contacttestresultcallback deepestnotmecontacttestresultcallback stepper movementsolver projectile
    actorconvexcallback raycasting mtphysics contacttestwrapper projectileconvexcallback
    )

add_openmw_dir (mwclass
    classes activator creature npc weapon armor potion apparatus book clothing container door
    ingredient creaturelevlist itemlevlist light lockpick misc probe repair static actor bodypart
    esm4base esm4activator esm4container esm4creature esm4npc esm4talkingactivator esm4terminal light4
    )

add_openmw_dir (mwmechanics
    mechanicsmanagerimp stat creaturestats magiceffects movement actorutil spelllist
    drawstate spells activespells npcstats aipackage aisequence aipursue alchemy aiwander aitravel aifollow aiavoiddoor aibreathe
    aicast aiescort aiface aiactivate aicombat recharge repair enchanting pathfinding pathgrid security spellcasting spellresistance
    disease pickpocket levelledlist combat steering obstacle autocalcspell difficultyscaling aicombataction summoning
    character dialoguefacing falloutcombat actors objects aistate weaponpriority spellpriority weapontype spellutil
    spelleffects
    )

add_openmw_dir (mwstate
    statemanagerimp charactermanager character quicksavemanager
    )

add_openmw_dir (mwbase
    environment world scriptmanager dialoguemanager journal soundmanager mechanicsmanager
    inputmanager windowmanager statemanager luamanager
    )

add_openmw_dir (mwvr
     openxrinput
     fnvxrliveframesurface
     radialmenu realisticcombat
     vranimation vrgui vrinputmanager vrinput vrlistbox vrmetamenu vrpingpongcallback vrpointer vrtracking vrtypes vrutil vrvirtualkeyboard
    )


# Main executable

add_library(openmw-lib STATIC
    ${OPENMW_FILES}
    ${OPENMW_SOURCES}
)

option(BUILD_OPENMW_SWG_LAB "Build isolated SWG lab targets" OFF)

if(BUILD_OPENMW_SWG_LAB)
    set(OPENMW_SWG_LAB_FILES ${OPENMW_FILES})
    set(OPENMW_SWG_LAB_MWRENDER_FILES)
    set(OPENMW_SWG_LAB_MWWORLD_FILES)
    foreach(source_file IN LISTS OPENMW_FILES)
        if(source_file MATCHES "^mwrender/.+\\.cpp$")
            list(REMOVE_ITEM OPENMW_SWG_LAB_FILES "${source_file}")
            if(NOT source_file STREQUAL "mwrender/objects.cpp"
                AND NOT source_file STREQUAL "mwrender/renderingmanager.cpp")
                list(APPEND OPENMW_SWG_LAB_MWRENDER_FILES "${source_file}")
            endif()
        elseif(source_file MATCHES "^mwworld/.+\\.cpp$")
            list(REMOVE_ITEM OPENMW_SWG_LAB_FILES "${source_file}")
            if(NOT source_file STREQUAL "mwworld/worldimp.cpp")
                list(APPEND OPENMW_SWG_LAB_MWWORLD_FILES "${source_file}")
            endif()
        elseif(source_file MATCHES "/ub_mwrender\\.cpp$" OR source_file MATCHES "/ub_mwworld\\.cpp$")
            list(REMOVE_ITEM OPENMW_SWG_LAB_FILES "${source_file}")
        endif()
    endforeach()
    set(OPENMW_SWG_LAB_MWRENDER_UNITY ${CMAKE_CURRENT_BINARY_DIR}/ub_swg_lab_mwrender.cpp)
    file(WRITE ${OPENMW_SWG_LAB_MWRENDER_UNITY} "// SWG lab unity build generated by CMake\n")
    foreach(source_file IN LISTS OPENMW_SWG_LAB_MWRENDER_FILES)
        file(APPEND ${OPENMW_SWG_LAB_MWRENDER_UNITY} "#include <${source_file}>\n")
    endforeach()
    set(OPENMW_SWG_LAB_MWWORLD_UNITY ${CMAKE_CURRENT_BINARY_DIR}/ub_swg_lab_mwworld.cpp)
    file(WRITE ${OPENMW_SWG_LAB_MWWORLD_UNITY} "// SWG lab unity build generated by CMake\n")
    foreach(source_file IN LISTS OPENMW_SWG_LAB_MWWORLD_FILES)
        file(APPEND ${OPENMW_SWG_LAB_MWWORLD_UNITY} "#include <${source_file}>\n")
    endforeach()
    list(APPEND OPENMW_SWG_LAB_FILES
        ${OPENMW_SWG_LAB_MWRENDER_UNITY}
        ${OPENMW_SWG_LAB_MWWORLD_UNITY}
        mwrender/objects_swg_lab.cpp
        mwrender/renderingmanager_swg_lab.cpp
        mwworld/worldimp_swg_lab.cpp
    )
    source_group("apps\\openmw\\swg-lab" FILES
        mwrender/objects_swg_lab.cpp
        mwrender/renderingmanager_swg_lab.cpp
        mwworld/worldimp_swg_lab.cpp
    )
    add_library(openmw-lib-swg-lab STATIC
        ${OPENMW_SWG_LAB_FILES}
        ${OPENMW_SOURCES}
    )
    target_compile_definitions(openmw-lib-swg-lab PRIVATE OPENMW_SWG_LAB_TARGET=1)
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

    # Workaround necessary to ensure osgAnimation::MatrixLinearSampler dynamic casts work under Clang
    # NOTE: it's unclear whether the broken behavior is spec-compliant
    if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
        set_target_properties(openmw PROPERTIES ENABLE_EXPORTS ON)
    endif()

    if(TARGET openmw-lib-swg-lab)
        openmw_add_executable(openmw_swg_lab
            ${APPLE_BUNDLE_RESOURCES}
            ${OPENMW_RESOURCES}
            main.cpp
        )
        target_link_libraries(openmw_swg_lab openmw-lib-swg-lab)
        target_compile_definitions(openmw_swg_lab PRIVATE OPENMW_SWG_LAB_TARGET=1)
    endif()
endif()

if(BUILD_OPENMW_VR)
    openmw_add_executable(openmw_vr
        ${APPLE_BUNDLE_RESOURCES}
        ${OPENMW_RESOURCES}
        main.cpp
    )
    target_link_libraries(openmw_vr openmw-lib)
    target_compile_definitions(openmw_vr PRIVATE OPENMW_VR=1)
    if(TARGET openmw-lib-swg-lab)
        openmw_add_executable(openmw_vr_swg_lab
            ${APPLE_BUNDLE_RESOURCES}
            ${OPENMW_RESOURCES}
            main.cpp
        )
        target_link_libraries(openmw_vr_swg_lab openmw-lib-swg-lab)
        target_compile_definitions(openmw_vr_swg_lab PRIVATE OPENMW_VR=1 OPENMW_SWG_LAB_TARGET=1)
    endif()
    #if (WIN32)
    #    target_compile_options(openmw_vr PUBLIC XR_USE_GRAPHICS_API_OPENGL XR_USE_GRAPHICS_API_D3D11 XR_USE_PLATFORM_WIN32)
    #elseif(UNIX)
    #    target_compile_options(openmw_vr PUBLIC XR_USE_GRAPHICS_API_OPENGL XR_USE_PLATFORM_XLIB XR_USE_PLATFORM_WAYLAND)
    #endif()
endif()

# Sound stuff - here so CMake doesn't stupidly recompile EVERYTHING
# when we change the backend.
include_directories(
    ${FFmpeg_INCLUDE_DIRS}
)

set(OPENMW_LIB_LINK_LIBRARIES
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
    "osg-ffmpeg-videoplayer"
    "oics"
    components
)
target_link_libraries(openmw-lib ${OPENMW_LIB_LINK_LIBRARIES})
if(TARGET openmw-lib-swg-lab)
    target_link_libraries(openmw-lib-swg-lab ${OPENMW_LIB_LINK_LIBRARIES})
endif()

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

add_definitions(-DMYGUI_DONT_USE_OBSOLETE=ON)

if (ANDROID)
    target_link_libraries(openmw-lib EGL android log z)
    if(TARGET openmw-lib-swg-lab)
        target_link_libraries(openmw-lib-swg-lab EGL android log z)
    endif()
endif (ANDROID)

if (USE_SYSTEM_TINYXML)
    target_link_libraries(openmw-lib ${TinyXML_LIBRARIES})
    if(TARGET openmw-lib-swg-lab)
        target_link_libraries(openmw-lib-swg-lab ${TinyXML_LIBRARIES})
    endif()
else()
    target_link_libraries(openmw-lib local_tinyxml)
    if(TARGET openmw-lib-swg-lab)
        target_link_libraries(openmw-lib-swg-lab local_tinyxml)
    endif()
endif()

if (NOT UNIX)
    target_link_libraries(openmw-lib ${SDL2MAIN_LIBRARY})
    if(TARGET openmw-lib-swg-lab)
        target_link_libraries(openmw-lib-swg-lab ${SDL2MAIN_LIBRARY})
    endif()
endif()

# Fix for not visible pthreads functions for linker with glibc 2.15
if (UNIX AND NOT APPLE)
    target_link_libraries(openmw-lib ${CMAKE_THREAD_LIBS_INIT})
    if(TARGET openmw-lib-swg-lab)
        target_link_libraries(openmw-lib-swg-lab ${CMAKE_THREAD_LIBS_INIT})
    endif()
endif()

if(APPLE AND (BUILD_OPENMW OR BUILD_OPENMW_VR))
    set(BUNDLE_RESOURCES_DIR "${APP_BUNDLE_DIR}/Contents/Resources")

    set(OPENMW_RESOURCES_ROOT ${BUNDLE_RESOURCES_DIR})

    add_subdirectory(../../files/ ${CMAKE_CURRENT_BINARY_DIR}/files)

    configure_file("${OpenMW_BINARY_DIR}/overrides.bin" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/defaults.bin" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/openmw.cfg" ${BUNDLE_RESOURCES_DIR} COPYONLY)
    configure_file("${OpenMW_BINARY_DIR}/gamecontrollerdb.txt" ${BUNDLE_RESOURCES_DIR} COPYONLY)

    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)

    ### VR_PATCH.
    #Use a commond list of link targets for openmw and openmw_vr
    set(OPENMW_LINK_TARGETS ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK})

    if (FFmpeg_FOUND)
        set(OPENMW_LINK_OPTIONS PRIVATE "LINKER:SHELL:-framework CoreVideo"
                                           "LINKER:SHELL:-framework CoreMedia"
                                           "LINKER:SHELL:-framework VideoToolbox"
                                           "LINKER:SHELL:-framework AudioToolbox"
                                           "LINKER:SHELL:-framework VideoDecodeAcceleration")
    endif()
    ### /VR_PATCH
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-lib PRIVATE --coverage)
    target_link_libraries(openmw-lib gcov)
    if (NOT ANDROID AND BUILD_OPENMW)
        ### VR_PATCH.
        #Use a commond list of link targets for openmw and openmw_vr
        set(OPENMW_COMPILE_OPTIONS PRIVATE --coverage)
        list(APPEND OPENMW_LINK_TARGETS gcov)
        ### /VR_PATCH
    endif()
endif()

### VR_PATCH
# apply options to both VR and non-VR
if(BUILD_OPENMW)
    target_link_libraries(openmw ${OPENMW_LINK_TARGETS})
    if(TARGET openmw_swg_lab)
        target_link_libraries(openmw_swg_lab ${OPENMW_LINK_TARGETS})
    endif()
    if(OPENMW_LINK_OPTIONS)
        target_link_options(openmw ${OPENMW_LINK_OPTIONS})
        if(TARGET openmw_swg_lab)
            target_link_options(openmw_swg_lab ${OPENMW_LINK_OPTIONS})
        endif()
    endif()
    if(OPENMW_COMPILE_OPTIONS)
        target_compile_options(openmw ${OPENMW_COMPILE_OPTIONS})
        if(TARGET openmw_swg_lab)
            target_compile_options(openmw_swg_lab ${OPENMW_COMPILE_OPTIONS})
        endif()
    endif()
    if(APPLE)
        add_custom_command(TARGET openmw
            POST_BUILD
            COMMAND cp "${OpenMW_BINARY_DIR}/resources/version" "${BUNDLE_RESOURCES_DIR}/resources")
    endif(APPLE)
endif()
if(BUILD_OPENMW_VR)
    target_link_libraries(openmw_vr ${OPENMW_LINK_TARGETS})
    if(TARGET openmw_vr_swg_lab)
        target_link_libraries(openmw_vr_swg_lab ${OPENMW_LINK_TARGETS})
    endif()
    if(OPENMW_LINK_OPTIONS)
        target_link_options(openmw_vr ${OPENMW_LINK_OPTIONS})
        if(TARGET openmw_vr_swg_lab)
            target_link_options(openmw_vr_swg_lab ${OPENMW_LINK_OPTIONS})
        endif()
    endif()
    if(OPENMW_COMPILE_OPTIONS)
        target_compile_options(openmw_vr ${OPENMW_COMPILE_OPTIONS})
        if(TARGET openmw_vr_swg_lab)
            target_compile_options(openmw_vr_swg_lab ${OPENMW_COMPILE_OPTIONS})
        endif()
    endif()
    if(APPLE)
        add_custom_command(TARGET openmw_vr
            POST_BUILD
            COMMAND cp "${OpenMW_BINARY_DIR}/resources/version" "${BUNDLE_RESOURCES_DIR}/resources")
        if(TARGET openmw_vr_swg_lab)
            add_custom_command(TARGET openmw_vr_swg_lab
                POST_BUILD
                COMMAND cp "${OpenMW_BINARY_DIR}/resources/version" "${BUNDLE_RESOURCES_DIR}/resources")
        endif()
    endif(APPLE)
    configure_resource_file(${OpenMW_SOURCE_DIR}/files/openxrinteractionprofiles.xml
        "${OpenMW_BINARY_DIR}" "openxrinteractionprofiles.xml")

endif()
### /VR_PATCH

if (WIN32 AND BUILD_OPENMW)
    INSTALL(TARGETS openmw RUNTIME DESTINATION ".")
    if(TARGET openmw_swg_lab)
        INSTALL(TARGETS openmw_swg_lab RUNTIME DESTINATION ".")
    endif()
endif()

if (WIN32 AND BUILD_OPENMW_VR)
    INSTALL(TARGETS openmw_vr RUNTIME DESTINATION ".")
    if(TARGET openmw_vr_swg_lab)
        INSTALL(TARGETS openmw_vr_swg_lab RUNTIME DESTINATION ".")
    endif()
endif()
