//
// Created by ivanl on 21.09.2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_GLWidget.h" resolved

#include "Widgets/glwidget.hpp"
#include "ui_GLWidget.h"
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include "Widgets/glmainwindow.hpp"
#include <exception>

namespace DesEngine
{
	GLWidget::GLWidget(QWidget *parent_widget, GLMainWindow* parent) :
		QOpenGLWidget(parent_widget), ui(new Ui::GLWidget)
	{
        _parent = dynamic_cast<GLMainWindow*>(parent);

        if (!_parent)
            throw std::runtime_error("Parent of GLWidget must be GLMainWindow");

		ui->setupUi(this);
	}

	GLWidget::~GLWidget()
	{
		delete ui;
	}

    void GLWidget::initializeGL()
    {
        auto&& funcs = *context()->functions();
        funcs.glClearColor(0, 0, 0, 1);

        funcs.glEnable(GL_DEPTH_TEST);
        funcs.glEnable(GL_CULL_FACE);
    }

    void GLWidget::paintGL()
    {
        auto funcs = *context()->functions();
        _parent->scene.draw(funcs);
    }

    void GLWidget::resizeGL(int w, int h)
    {
        auto funcs = *context()->functions();

        float aspect = w / float(h);
        _parent->scene.set_aspect_ratio(aspect);

        funcs.glViewport(0, 0, w, h);
        _parent->scene.update();
    }
} // DesEngine
