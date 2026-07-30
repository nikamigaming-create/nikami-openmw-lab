set(OCCLUSIONCULLING_SOURCE_FILES
    occlusiondb.cpp
)

set(OCCLUSIONCULLING_HEADER_FILES
    occlusiondb.hpp
    types.hpp
)

# These files are compiled into the shared 'components' target via the
# parent CMakeLists.txt's add_component_dir() macro.
# If your project uses explicit component listing, add occlusionculling
# to that list instead.
add_component_dir(occlusionculling
    occlusiondb pvscache contenthash types
)
