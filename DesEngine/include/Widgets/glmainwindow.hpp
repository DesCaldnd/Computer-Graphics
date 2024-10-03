//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_
#define DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_

#include <QMainWindow>
#include "../Classes/Scene.hpp"

namespace DesEngine
{
    class GLWidget;

	QT_BEGIN_NAMESPACE
	namespace Ui { class GLMainWindow; }
	QT_END_NAMESPACE

	class GLMainWindow : public QMainWindow
	{
	Q_OBJECT

	public:
		explicit GLMainWindow(QWidget *parent = nullptr);

		~GLMainWindow() override;

        Scene scene;
        GLWidget* glwidget;



    public slots:
        void slot_shortcut_windowstate();

	private:
		Ui::GLMainWindow *ui;
	};
} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_
