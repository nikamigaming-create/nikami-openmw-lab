set(NIFTEST
    niftest.cpp
)
source_group(components\\nif\\tests FILES ${NIFTEST})

# Main executable
openmw_add_executable(niftest
    ${NIFTEST}
)

target_link_libraries(niftest
    components
)

<<<<<<< HEAD
if (WIN32)
    target_sources(niftest PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

=======
>>>>>>> origin/main
if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(niftest PRIVATE --coverage)
    target_link_libraries(niftest gcov)
endif()

if (WIN32)
    install(TARGETS niftest RUNTIME DESTINATION ".")
endif()

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
<<<<<<< HEAD
    target_precompile_headers(niftest REUSE_FROM components)
=======
    target_precompile_headers(niftest PRIVATE <filesystem>)
>>>>>>> origin/main
endif()
