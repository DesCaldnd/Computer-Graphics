
add_executable(Test_ui main.cpp testwindow.cpp testwindow.hpp)

target_link_libraries(Test_ui
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::OpenGLWidgets
)
