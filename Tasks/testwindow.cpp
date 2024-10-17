//
// Created by Des Caldnd on 10/17/2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_TestWindow.h" resolved

#include "testwindow.hpp"
#include "ui_TestWindow.h"


TestWindow::TestWindow(QWidget *parent) :
        QMainWindow(parent), ui(new Ui::TestWindow)
{
    ui->setupUi(this);
}

TestWindow::~TestWindow()
{
    delete ui;
}
