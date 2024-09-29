//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_
#define DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_

#include <QOpenGLWidget>

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

    private:
		Ui::GLWidget *ui;
        GLMainWindow* _parent;
	};
} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_
