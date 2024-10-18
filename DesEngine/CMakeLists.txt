add_library(DesEngine
        include/Interfaces/Transformable.hpp
        include/Interfaces/Serializable.hpp
        include/Interfaces/Drawable.hpp
        include/Interfaces/Editable.hpp
        include/Interfaces/Playable.hpp
        include/Interfaces/Physicable.hpp
        include/Interfaces/LogicObject.hpp
        source/Classes/MeshObject.cpp
        include/Classes/MeshObject.hpp
        source/Widgets/glwidget.cpp
        include/Widgets/glwidget.hpp
        source/Widgets/glwidget.ui
        source/Widgets/glmainwindow.cpp
        include/Widgets/glmainwindow.hpp
        source/Classes/LightObject.cpp
        include/Classes/LightObject.hpp source/Classes/GroupObject.cpp include/Classes/GroupObject.hpp source/Classes/CameraObject.cpp include/Classes/CameraObject.hpp source/Classes/GameMode.cpp include/Classes/GameMode.hpp source/Classes/Material.cpp include/Classes/Material.hpp source/Classes/Scene.cpp include/Classes/Scene.hpp source/Classes/Serializable.cpp include/Classes/BasicTypes.hpp
        source/Classes/SkyBoxObject.cpp
        include/Classes/SkyBoxObject.hpp
        source/Classes/LogicObject.cpp
        include/Classes/Utils.hpp
        source/Classes/Utils.cpp
        source/Widgets/propertywidget.cpp
        include/Widgets/propertywidget.hpp
        source/Widgets/propertywidget.ui
)

qt_add_resources(DesEngine "additional_files"
        PREFIX "/"
        FILES

        Shaders/pbr.fsh
        Shaders/pbr.vsh
        Shaders/depth.fsh
        Shaders/depth.vsh
        Shaders/color_index.fsh
        Shaders/color_index.vsh

        Primitives/albedo.png
        Primitives/cube.mtl
        Primitives/cube.obj
        Primitives/cubes.mtl
        Primitives/cubes.obj
        Primitives/monkey.mtl
        Primitives/monkey.obj
        Primitives/monkeys.mtl
        Primitives/monkeys.obj
        Primitives/realmat.mtl
        Primitives/realmat.obj
)

target_include_directories(DesEngine PUBLIC ./include)
target_link_libraries(DesEngine PUBLIC
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::OpenGLWidgets
        )
target_link_libraries(DesEngine PUBLIC ${BULLET_LIBRARIES})
target_link_libraries(
        DesEngine PUBLIC
        nlohmann_json::nlohmann_json)

set(DESENGINE_LIB_DIR "${CMAKE_SOURCE_DIR}/DesEngine/" CACHE PATH "DESENGINE_LIB_DIR")

add_custom_command(
        TARGET DesEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/DesEngine/Shaders
        ${CMAKE_CURRENT_BINARY_DIR}/Shaders)

add_custom_command(
        TARGET DesEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/DesEngine/Primitives
        ${CMAKE_CURRENT_BINARY_DIR}/Primitives)