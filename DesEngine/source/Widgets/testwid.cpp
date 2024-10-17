//
// Created by Des Caldnd on 10/17/2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_testwid.h" resolved

#include "Widgets/testwid.hpp"
#include "ui_testwid.h"

namespace DesEngine
{
    testwid::testwid(QWidget *parent) :
            QWidget(parent), ui(new Ui::testwid)
    {
        ui->setupUi(this);
    }

    testwid::~testwid()
    {
        delete ui;
    }
} // DesEngine
