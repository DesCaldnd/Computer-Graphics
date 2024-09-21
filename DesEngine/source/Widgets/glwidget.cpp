//
// Created by ivanl on 21.09.2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_GLWidget.h" resolved

#include "Widgets/glwidget.hpp"
#include "ui_GLWidget.h"

namespace DesEngine
{
	GLWidget::GLWidget(QWidget *parent) :
		QOpenGLWidget(parent), ui(new Ui::GLWidget)
	{
		ui->setupUi(this);
	}

	GLWidget::~GLWidget()
	{
		delete ui;
	}
} // DesEngine
