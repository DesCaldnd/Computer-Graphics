//
// Created by Des Caldnd on 10/17/2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_PropertyWidget.h" resolved

#include "Widgets/propertywidget.hpp"
#include "ui_PropertyWidget.h"

namespace DesEngine
{
    PropertyWidget::PropertyWidget(QWidget *parent) :
            QWidget(parent), ui(new Ui::PropertyWidget)
    {
        ui->setupUi(this);
        ui->scrollAreaWidgetContents->layout()->setAlignment(Qt::AlignmentFlag::AlignTop);
    }

    PropertyWidget::~PropertyWidget()
    {
        delete ui;
    }
} // DesEngine
