set(OSGQT_LIBRARY "osgQt")

# Sources

set(SOURCE_H
<<<<<<< HEAD
    include/osgQt/osgQOpenGLWidget.hpp
    include/osgQt/CompositeOsgRenderer.hpp
=======
    osgQOpenGLWidget.hpp
    CompositeOsgRenderer.hpp
>>>>>>> origin/main
)

qt_wrap_cpp(SOURCES_H_MOC ${SOURCE_H} )

set(OSGQT_SOURCE_FILES
    osgQOpenGLWidget.cpp
    CompositeOsgRenderer.cpp
    ${SOURCES_H_MOC}
)

add_library(${OSGQT_LIBRARY} STATIC ${OSGQT_SOURCE_FILES})
<<<<<<< HEAD
target_include_directories(${OSGQT_LIBRARY} SYSTEM PUBLIC include)
target_include_directories(${OSGQT_LIBRARY} PRIVATE include/osgQt)

target_link_libraries(${OSGQT_LIBRARY} Qt::Core Qt::OpenGL Qt::OpenGLWidgets)

=======

target_link_libraries(${OSGQT_LIBRARY} Qt::Core Qt::OpenGL Qt::OpenGLWidgets)

link_directories(${CMAKE_CURRENT_BINARY_DIR})

>>>>>>> origin/main
set(EXTERN_OSGQT_LIBRARY ${OSGQT_LIBRARY} PARENT_SCOPE)
