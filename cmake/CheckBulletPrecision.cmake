set(TMP_ROOT ${CMAKE_BINARY_DIR}/try-compile)
file(MAKE_DIRECTORY ${TMP_ROOT})

file(WRITE ${TMP_ROOT}/checkbullet.cpp
"
#include <BulletCollision/CollisionShapes/btSphereShape.h>
int main(int argc, char** argv)
{
    btSphereShape shape(1.0);
    btScalar mass(1.0);
    btVector3 inertia;
    shape.calculateLocalInertia(mass, inertia);
    return 0;
}
")

message(STATUS "Checking if Bullet uses double precision")

if(ANDROID)
    # Android try_compile link probes are brittle in this tree even when the
    # configured Bullet headers/libs compile and link with the NDK toolchain.
    set(HAS_DOUBLE_PRECISION_BULLET TRUE)
else()
    try_compile(RESULT_VAR
        ${TMP_ROOT}/temp
        ${TMP_ROOT}/checkbullet.cpp
        COMPILE_DEFINITIONS "-DBT_USE_DOUBLE_PRECISION"
        LINK_LIBRARIES ${BULLET_LIBRARIES}
        CMAKE_FLAGS  "-DINCLUDE_DIRECTORIES=${BULLET_INCLUDE_DIRS}"
        )
    set(HAS_DOUBLE_PRECISION_BULLET ${RESULT_VAR})
endif()
