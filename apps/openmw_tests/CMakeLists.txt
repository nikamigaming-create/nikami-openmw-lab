include_directories(SYSTEM ${GTEST_INCLUDE_DIRS})
include_directories(SYSTEM ${GMOCK_INCLUDE_DIRS})

file(GLOB UNITTEST_SRC_FILES
    main.cpp

    options.cpp

    mwworld/teststore.cpp
    mwworld/testesm4questruntime.cpp
    mwworld/testesm4actorvaluestore.cpp
    mwworld/testesm4factionstore.cpp
    mwworld/testesm4perkstore.cpp
    mwworld/testesm4recipestore.cpp
    mwworld/testesm4notestore.cpp
    mwworld/testesm4specialinventorystore.cpp
    mwworld/testfnvcraftingruntime.cpp
    mwworld/testfnvlockpickruntime.cpp
    mwworld/testactionfnvcrafting.cpp
    mwworld/testfnvradioprogram.cpp
    mwworld/testfnvterminalruntime.cpp
    mwworld/testfnvplayerstate.cpp
    mwworld/testfnvplayerruntimestate.cpp
    mwworld/testfnvweather.cpp
    mwworld/testfnvfasttravel.cpp
    mwworld/testduration.cpp
    mwworld/testtimestamp.cpp
    mwworld/testptr.cpp
    mwworld/testcellref.cpp
    mwworld/testcellvisitors.cpp
    mwworld/testactorfacing.cpp
    mwworld/testfnvtriggervolume.cpp
    mwworld/testcontainerstoreesm4.cpp
    mwworld/testweather.cpp

    mwdialogue/testkeywordsearch.cpp
    mwdialogue/testesm4dialogueutils.cpp
    mwdialogue/testesm4resultscript.cpp

    mwgui/tooltips.cpp
    mwgui/weightedsearch.cpp

    mwinput/testcontrolswitch.cpp

    mwclass/testesm4container.cpp
    mwclass/testesm4talkingactivator.cpp
    mwclass/testfnvfurniturelifecycle.cpp
    mwclass/testfnvidleonlypackage.cpp
    mwclass/testfnvsandbox.cpp

    mwrender/testfalloutweaponanimation.cpp
    mwrender/testobjectpaging.cpp
    mwrender/testplayervisualpolicy.cpp

    mwmechanics/testfalloutcombat.cpp
    mwmechanics/testdialoguefacing.cpp
    mwmechanics/testownership.cpp

    mwsound/testsaysequence.cpp
    mwsound/testsoundbuffer.cpp

    mwscript/testscripts.cpp
)

if (MSVC)
    # these TUs take a lot of memory for codegen
    # in CI, they often go over the 8GB memory limit if another cl.exe is running at the same time
    # this option (make sure you're not looking at the docs for the identically-named linker option) stops four codegen threads being spun off
    # it hurts build time ~33% to enable this unnecessarily, though, so target it to the hungriest boys
    set_source_files_properties(
        mwworld/teststore.cpp
        PROPERTIES
        COMPILE_OPTIONS /cgthreads1
    )
endif()

source_group(apps\\openmw-tests FILES ${UNITTEST_SRC_FILES})

openmw_add_executable(openmw-tests ${UNITTEST_SRC_FILES})

target_link_libraries(openmw-tests
    GTest::GTest
    GMock::GMock
    openmw-lib
)

# Fix for not visible pthreads functions for linker with glibc 2.15
if (UNIX AND NOT APPLE)
    target_link_libraries(openmw-tests ${CMAKE_THREAD_LIBS_INIT})
endif()

if (WIN32)
    target_sources(openmw-tests PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(openmw-tests PRIVATE --coverage)
    target_link_libraries(openmw-tests gcov)
endif()

target_compile_definitions(openmw-tests
    PRIVATE OPENMW_DATA_DIR=u8"${CMAKE_CURRENT_BINARY_DIR}/data"
            OPENMW_PROJECT_SOURCE_DIR=u8"${PROJECT_SOURCE_DIR}")

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(openmw-tests PRIVATE
        <boost/program_options/options_description.hpp>

        <gtest/gtest.h>

        <sol/sol.hpp>

        <algorithm>
        <filesystem>
        <fstream>
        <functional>
        <memory>
        <string>
        <vector>
    )
endif()
