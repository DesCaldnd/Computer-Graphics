//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_
#define DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_

#include <QOpenGLWidget>

namespace DesEngine
{
	QT_BEGIN_NAMESPACE
	namespace Ui { class GLWidget; }
	QT_END_NAMESPACE

	class GLWidget : public QOpenGLWidget
	{
	Q_OBJECT

	public:
		explicit GLWidget(QWidget *parent = nullptr);

		~GLWidget() override;

	private:
		Ui::GLWidget *ui;
	};
} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLWIDGET_HPP_
