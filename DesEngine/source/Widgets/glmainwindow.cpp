//
// Created by ivanl on 21.09.2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_GLMainWindow.h" resolved

#include "Widgets/glmainwindow.hpp"
#include "ui_GLMainWindow.h"
#include "Widgets/glwidget.hpp"
#include "QShortcut"

namespace DesEngine
{
	GLMainWindow::GLMainWindow(QWidget *parent) :
		QMainWindow(parent), ui(new Ui::GLMainWindow), scene(this)
	{
		ui->setupUi(this);

        glwidget = new GLWidget(this);
        ui->hlayout->addWidget(glwidget);


        QShortcut* shortcut2 = new QShortcut(QKeySequence(Qt::Key_F11), this);
        shortcut2->setAutoRepeat(false);

        connect(shortcut2,    &QShortcut::activated,
                         this,        &GLMainWindow::slot_shortcut_windowstate);
	}

	GLMainWindow::~GLMainWindow()
	{
		delete ui;
	}

    void GLMainWindow::slot_shortcut_windowstate()
    {
        if (isFullScreen())
        {
            showNormal();
        } else
        {
            showFullScreen();
        }
    }

    void GLMainWindow::mousePressEvent(QMouseEvent *event)
    {
        emit mousePressSignal(event);
    }

    void GLMainWindow::mouseReleaseEvent(QMouseEvent *event)
    {
        emit mouseReleaseSignal(event);
    }

    void GLMainWindow::mouseDoubleClickEvent(QMouseEvent *event)
    {
        emit mouseDoubleClickSignal(event);
    }

    void GLMainWindow::mouseMoveEvent(QMouseEvent *event)
    {
        emit mouseMoveSignal(event);
    }

    void GLMainWindow::wheelEvent(QWheelEvent *event)
    {
        emit wheelSignal(event);
    }

    void GLMainWindow::keyPressEvent(::QKeyEvent *event)
    {
        emit keyPressSignal(event);
    }

    void GLMainWindow::keyReleaseEvent(::QKeyEvent *event)
    {
        emit keyReleaseSignal(event);
    }
} // DesEngine
