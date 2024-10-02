//
// Created by ivanl on 21.09.2024.
//

#ifndef DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_
#define DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_

#include <QMainWindow>
#include "../Classes/Scene.hpp"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

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
        void mousePressEvent(QMouseEvent *event) override;

        void mouseReleaseEvent(QMouseEvent *event) override;

        void mouseDoubleClickEvent(QMouseEvent *event) override;

        void mouseMoveEvent(QMouseEvent *event) override;

        void wheelEvent(QWheelEvent *event) override;

        void keyPressEvent(::QKeyEvent *event) override;

        void keyReleaseEvent(::QKeyEvent *event) override;

    signals:

        void mousePressSignal(QMouseEvent *event);

        void mouseReleaseSignal(QMouseEvent *event);

        void mouseDoubleClickSignal(QMouseEvent *event);

        void mouseMoveSignal(QMouseEvent *event);

        void wheelSignal(QWheelEvent *event);

        void keyPressSignal(::QKeyEvent *event);

        void keyReleaseSignal(::QKeyEvent *event);

    public slots:
        void slot_shortcut_windowstate();

	private:
		Ui::GLMainWindow *ui;
	};
} // DesEngine

#endif //DESENGINE_DESENGINE_INCLUDE_WIDGETS_GLMAINWINDOW_HPP_
