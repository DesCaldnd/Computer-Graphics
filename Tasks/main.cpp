//
// Created by Des Caldnd on 10/17/2024.
//
#include <QApplication>
#include "testwindow.hpp"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);



    TestWindow window;
    window.show();

    return QApplication::exec();
}
