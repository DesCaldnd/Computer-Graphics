//
// Created by Des Caldnd on 10/17/2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_PropertyWidget.h" resolved

#include "Widgets/propertywidget.hpp"
#include "ui_PropertyWidget.h"
#include "Classes/Scene.hpp"
#include "Widgets/property_ui.hpp"

namespace DesEngine
{
    PropertyWidget::PropertyWidget(Scene* scene, QWidget *parent) :
            QWidget(parent), ui(new Ui::PropertyWidget)
    {
        ui->setupUi(this);
        ui->scrollAreaWidgetContents->layout()->setAlignment(Qt::AlignmentFlag::AlignTop);
        ui->scrollAreaWidgetContents_2->layout()->setAlignment(Qt::AlignmentFlag::AlignTop);
        ui->property_layout->setAlignment(Qt::AlignmentFlag::AlignTop);
        ui->camera_data_layout->setAlignment(Qt::AlignmentFlag::AlignTop);
        ui->class_layout->setAlignment(Qt::AlignmentFlag::AlignTop);

        ui->camera_id_label->setText(QString::number(scene->get_game_camera()->get_id()));
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
        ui->object_class_label->setText(obj->get_class_name().c_str());

        auto props = obj->get_properties();

        for(auto&& prop : props)
        {
            property_ui* pui = new property_ui(prop, obj.get(), this);

            ui->property_layout->addWidget(pui);
        }

        connect(ui->remove_obj_button, &QPushButton::clicked, [this](bool){ _obj->remove(); unselect_object(); });
        ui->id_number_label->setText(QString::number(_obj->get_id()));
    }

    void PropertyWidget::unselect_object()
    {
        if (!_obj)
            return;

        ui->remove_obj_button->disconnect();

        QLayoutItem *child;
        while ((child = ui->property_layout->takeAt(0)) != nullptr) {
            delete child->widget(); // delete the widget
            delete child;   // delete the layout item
        }

        ui->id_number_label->setText(QString::number(0));
        ui->object_class_label->setText("none");

        _obj = nullptr;
    }

    void PropertyWidget::change_camera(id_t id)
    {
        ui->camera_id_label->setText(QString::number(id));
    }
} // DesEngine
