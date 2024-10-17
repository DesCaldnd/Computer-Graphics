//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_
#define DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_

#include <QMainWindow>
#include <QTabWidget>

#include "../Classes/Scene.hpp"

namespace DesEngine
{
    class GLWidget;
    class PropertyWidget;

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

        friend class Scene;


    public slots:
        void slot_shortcut_windowstate();
        void slot_save_scene_dialog();
        void slot_open_scene_dialog();

    private slots:


	private:

        PropertyWidget* _tab;

		Ui::GLMainWindow *ui;
	};
} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_
