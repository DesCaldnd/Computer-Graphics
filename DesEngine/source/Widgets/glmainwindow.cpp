//
// Created by ivanl on 21.09.2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_GLMainWindow.h" resolved

#include "Widgets/glmainwindow.hpp"
#include "ui_GLMainWindow.h"

namespace DesEngine
{
	GLMainWindow::GLMainWindow(QWidget *parent) :
		QMainWindow(parent), ui(new Ui::GLMainWindow)
	{
		ui->setupUi(this);
	}

	GLMainWindow::~GLMainWindow()
	{
		delete ui;
	}
} // DesEngine
