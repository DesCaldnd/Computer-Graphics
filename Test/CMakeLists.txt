
add_executable(Test main.cpp
        source/Bezier3.cpp
        include/Bezier3.hpp)

target_link_libraries(Test
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::OpenGLWidgets
        )
target_link_libraries(Test DesEngine)

add_custom_command(
        TARGET Test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${DESENGINE_LIB_DIR}/Shaders
        ${CMAKE_CURRENT_BINARY_DIR}/Shaders)

add_custom_command(
        TARGET Test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders
        ${CMAKE_CURRENT_BINARY_DIR}/Shaders)

add_custom_command(
        TARGET Test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${DESENGINE_LIB_DIR}/Primitives
        ${CMAKE_CURRENT_BINARY_DIR}/Primitives)
