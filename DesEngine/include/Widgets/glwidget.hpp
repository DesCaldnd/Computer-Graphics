//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_
#define DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_

#include <QOpenGLWidget>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace DesEngine
{
    class GLMainWindow;
	QT_BEGIN_NAMESPACE
	namespace Ui { class GLWidget; }
	QT_END_NAMESPACE

	class GLWidget : public QOpenGLWidget
	{
	Q_OBJECT

	public:
		explicit GLWidget(GLMainWindow* parent);

		~GLWidget() override;

    protected:

        void initializeGL() override;
        void paintGL() override;
        void resizeGL(int w, int h) override;

    protected slots:
        void mousePressEvent(QMouseEvent *event) override;

        void mouseReleaseEvent(QMouseEvent *event) override;

        void mouseDoubleClickEvent(QMouseEvent *event) override;

        void mouseMoveEvent(QMouseEvent *event) override;

        void wheelEvent(QWheelEvent *event) override;

        void keyPressEvent(::QKeyEvent *event) override;

        void keyReleaseEvent(::QKeyEvent *event) override;

    signals:

        void mousePressSignal(::QMouseEvent *event);

        void mouseReleaseSignal(::QMouseEvent *event);

        void mouseDoubleClickSignal(::QMouseEvent *event);

        void mouseMoveSignal(::QMouseEvent *event);

        void wheelSignal(::QWheelEvent *event);

        void keyPressSignal(::QKeyEvent *event);

        void keyReleaseSignal(::QKeyEvent *event);

    private:
		Ui::GLWidget *ui;
        GLMainWindow* _parent;
	};
} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_
