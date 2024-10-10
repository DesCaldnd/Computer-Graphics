//
// Created by ivanl on 21.09.2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_GLMainWindow.h" resolved

#include "Widgets/glmainwindow.hpp"
#include "ui_GLMainWindow.h"
#include "Widgets/glwidget.hpp"
#include <QShortcut>
#include <QFileDialog>

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

    void GLMainWindow::slot_save_scene_dialog()
    {
        QString filepath = QFileDialog::getSaveFileName(this, tr("Save scene"), "./untitled.desd", tr("DesEngine Scene Description file (*.desd)"));

        if (!filepath.isEmpty())
        {
            scene.save_to_file(filepath.toStdString());
        }
    }

    void GLMainWindow::slot_open_scene_dialog()
    {
        QString filepath = QFileDialog::getOpenFileName(this, tr("Open scene"), "./", tr("DesEngine Scene Description file (*.desd)"));

        if (filepath.isEmpty())
            return;

        bool e = scene.is_in_edit();

        scene.clear();
        scene.init();
        scene.load_from_file(filepath.toStdString(), e);
    }

} // DesEngine
