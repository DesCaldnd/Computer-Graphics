add_library(DesEngine SHARED
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
        include/Widgets/glwidget.ui
        source/Widgets/glmainwindow.cpp
        include/Widgets/glmainwindow.hpp
        include/Widgets/glmainwindow.ui
        source/Classes/LightObject.cpp
        include/Classes/LightObject.hpp source/Classes/GroupObject.cpp include/Classes/GroupObject.hpp source/Classes/CameraObject.cpp include/Classes/CameraObject.hpp source/Classes/GameMode.cpp include/Classes/GameMode.hpp source/Classes/Material.cpp include/Classes/Material.hpp source/Classes/Scene.cpp include/Classes/Scene.hpp source/Classes/Serializable.cpp include/Classes/BasicTypes.hpp)

target_include_directories(DesEngine PUBLIC ./include)
target_link_libraries(DesEngine
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::OpenGLWidgets
        )