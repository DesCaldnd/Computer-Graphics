//
// Created by Des Caldnd on 10/17/2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_PropertyWidget.h" resolved

#include "Widgets/propertywidget.hpp"
#include "ui_PropertyWidget.h"
#include "Classes/Scene.hpp"

namespace DesEngine
{
    PropertyWidget::PropertyWidget(Scene* scene, QWidget *parent) :
            QWidget(parent), ui(new Ui::PropertyWidget)
    {
        ui->setupUi(this);
        ui->scrollAreaWidgetContents->layout()->setAlignment(Qt::AlignmentFlag::AlignTop);
        ui->scrollAreaWidgetContents_2->layout()->setAlignment(Qt::AlignmentFlag::AlignTop);
        ui->property_layout->setAlignment(Qt::AlignmentFlag::AlignTop);

        ui->id_number_label->setText(QString::number(0));

        connect(ui->add_ob_button, &QPushButton::clicked, [scene](bool){ scene->add_object_dialog(); });
        connect(ui->register_light_button, &QPushButton::clicked, [scene](bool){ scene->register_light_dialog(); });
        connect(ui->unregister_light_button, &QPushButton::clicked, [scene](bool){ scene->unregister_light_dialog(); });
        connect(ui->load_gamemode, &QPushButton::clicked, [scene](bool){ scene->load_gamemode_dialog(); });
        connect(ui->set_camera_button, &QPushButton::clicked, [scene](bool){ scene->set_camera_dialog(); });
    }

    PropertyWidget::~PropertyWidget()
    {
        delete ui;
    }

    void PropertyWidget::select_object(std::shared_ptr<LogicObject> obj)
    {
        unselect_object();

        _obj = obj.get();

        connect(ui->remove_obj_button, &QPushButton::clicked, [this](bool){ _obj->remove(); unselect_object(); });
        ui->id_number_label->setText(QString::number(_obj->get_id()));
    }

    void PropertyWidget::unselect_object()
    {
        if (!_obj)
            return;

        ui->remove_obj_button->disconnect();

        while (ui->property_layout->count() > 0)
        {
            if (ui->property_layout->takeAt(0)->layout())
                ui->property_layout->takeAt(0)->layout()->deleteLater();
            else if(ui->property_layout->takeAt(0)->widget())
                ui->property_layout->takeAt(0)->widget()->deleteLater();
        }

        ui->id_number_label->setText(QString::number(0));

        _obj = nullptr;
    }
} // DesEngine
