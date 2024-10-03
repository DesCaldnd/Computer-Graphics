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
	GLWidget::GLWidget(GLMainWindow* parent) : ui(new Ui::GLWidget)
	{
        _parent = dynamic_cast<GLMainWindow*>(parent);

        if (!_parent)
            throw std::runtime_error("Parent of GLWidget must be GLMainWindow");

		ui->setupUi(this);
//        setUpdateBehavior(QOpenGLWidget::UpdateBehavior::PartialUpdate);
        setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    }


	GLWidget::~GLWidget()
	{
		delete ui;
	}

    void GLWidget::initializeGL()
    {
        auto&& funcs = *context()->functions();

        funcs.initializeOpenGLFunctions();

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

    void GLWidget::mousePressEvent(::QMouseEvent *event)
    {
        emit mousePressSignal(event);
    }

    void GLWidget::mouseReleaseEvent(::QMouseEvent *event)
    {
        emit mouseReleaseSignal(event);
    }

    void GLWidget::mouseDoubleClickEvent(::QMouseEvent *event)
    {
        emit mouseDoubleClickSignal(event);
    }

    void GLWidget::mouseMoveEvent(::QMouseEvent *event)
    {
        emit mouseMoveSignal(event);
    }

    void GLWidget::wheelEvent(::QWheelEvent *event)
    {
        emit wheelSignal(event);
    }

    void GLWidget::keyPressEvent(::QKeyEvent *event)
    {
        emit keyPressSignal(event);
    }

    void GLWidget::keyReleaseEvent(::QKeyEvent *event)
    {
        emit keyReleaseSignal(event);
    }

} // DesEngine
