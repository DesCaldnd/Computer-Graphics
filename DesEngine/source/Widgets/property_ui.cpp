//
// Created by Des Caldnd on 10/18/2024.
//

// You may need to build the project (run Qt uic code generator) to get "ui_property_ui.h" resolved

#include "Widgets/property_ui.hpp"
#include "ui_property_ui.h"
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QValidator>

namespace DesEngine
{
    property_ui::property_ui(const property_t& prop, Editable* obj, QWidget *parent) :
            QWidget(parent), ui(new Ui::property_ui), _prop(prop), _obj(obj)
    {
        ui->setupUi(this);
        ui->layout->setAlignment(Qt::AlignmentFlag::AlignTop);

        ui->label->setText(_prop.name.c_str());

        QWidget* wid = nullptr;

        switch(_prop.type)
        {

            case property_data_type_t::CALL:
            {
                QPushButton* button = new QPushButton(this);

                ui->label->hide();
                button->setText(_prop.name.c_str());
                connect(button, &QPushButton::clicked, [this](bool){ _prop.setter(_obj, property_data_t()); });

                wid = button;
                break;
            }
            case property_data_type_t::INT:
            {
                QLineEdit* line = new QLineEdit(this);


                QRegularExpressionValidator* val = new QRegularExpressionValidator(QRegularExpression{"^\\-?[0-9]*$"}, this);
                line->setValidator(val);
                line->setPlaceholderText("Enter int:");
                line->setText(QString::number(std::get<int>(_prop.getter(_obj))));

                connect(line, &QLineEdit::returnPressed, [this](){
                   auto text = std::get<QLineEdit*>(_iw)->text().toStdString();
                   int res;
                   try
                   {
                       res = std::stoi(text);
                   } catch(...)
                   {
                       return;
                   }

                   bool b = _prop.setter(_obj, res);

                   if (b)
                       std::get<QLineEdit*>(_iw)->setText(QString::number(std::get<int>(_prop.getter(_obj))));
                });

                _iw = line;
                wid = line;

                break;
            }
            case property_data_type_t::UINT:
            {
                QLineEdit* line = new QLineEdit(this);


                QRegularExpressionValidator* val = new QRegularExpressionValidator(QRegularExpression{"^[0-9]*$"}, this);
                line->setValidator(val);
                line->setPlaceholderText("Enter uint:");
                line->setText(QString::number(std::get<unsigned int>(_prop.getter(_obj))));

                connect(line, &QLineEdit::returnPressed, [this](){
                    auto text = std::get<QLineEdit*>(_iw)->text().toStdString();
                    unsigned int res;
                    try
                    {
                        res = std::stoul(text);
                    } catch(...)
                    {
                        return;
                    }

                    bool b = _prop.setter(_obj, res);

                    if (b)
                        std::get<QLineEdit*>(_iw)->setText(QString::number(std::get<unsigned int>(_prop.getter(_obj))));
                });

                _iw = line;
                wid = line;

                break;
            }
            case property_data_type_t::FLOAT:
            {
                QLineEdit* line = new QLineEdit(this);


                QRegularExpressionValidator* val = new QRegularExpressionValidator(QRegularExpression{"^\\-?[0-9]*(.[0-9]+)?$"}, this);
                line->setValidator(val);
                line->setPlaceholderText("Enter float:");
                line->setText(QString::number(std::get<float>(_prop.getter(_obj))));

                connect(line, &QLineEdit::returnPressed, [this](){
                    auto text = std::get<QLineEdit*>(_iw)->text().toStdString();
                    float res;
                    try
                    {
                        res = std::stof(text);
                    } catch(...)
                    {
                        return;
                    }

                    bool b = _prop.setter(_obj, res);

                    if (b)
                        std::get<QLineEdit*>(_iw)->setText(QString::number(std::get<float>(_prop.getter(_obj))));
                });

                _iw = line;
                wid = line;
                break;
            }
            case property_data_type_t::STRING:
            {
                QLineEdit* line = new QLineEdit(this);

                line->setPlaceholderText("Enter uint:");
                line->setText(std::get<std::string>(_prop.getter(_obj)).c_str());

                connect(line, &QLineEdit::returnPressed, [this](){
                    auto text = std::get<QLineEdit*>(_iw)->text().toStdString();

                    bool b = _prop.setter(_obj, text);

                    if (b)
                        std::get<QLineEdit*>(_iw)->setText(std::get<std::string>(_prop.getter(_obj)).c_str());
                });

                _iw = line;
                wid = line;
                break;
            }
            case property_data_type_t::BOOLEAN:
            {
                QCheckBox* box = new QCheckBox(this);

                box->setCheckState(std::get<bool>(_prop.getter(_obj)) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

                connect(box, &QCheckBox::stateChanged, [this](int state){
                    bool res = state;

                    bool b = _prop.setter(_obj, res);

                    if (b)
                        std::get<QCheckBox*>(_iw)->setCheckState(std::get<bool>(_prop.getter(_obj)) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
                });

                _iw = box;
                wid = box;

                break;
            }
        }

        if (wid)
        {
            wid->setSizePolicy(QSizePolicy::Policy::Minimum, QSizePolicy::Policy::Minimum);
            ui->layout->addWidget(wid);
        }
    }

    property_ui::~property_ui()
    {
        delete ui;
    }
} // DesEngine
