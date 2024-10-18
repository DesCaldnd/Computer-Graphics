//
// Created by Des Caldnd on 10/18/2024.
//

#ifndef DESENGINE_PROPERTY_UI_HPP
#define DESENGINE_PROPERTY_UI_HPP

#include <QWidget>
#include "Interfaces/Editable.hpp"

class QLineEdit;
class QCheckBox;

namespace DesEngine
{
    QT_BEGIN_NAMESPACE
    namespace Ui
    {
        class property_ui;
    }
    QT_END_NAMESPACE

    class property_ui : public QWidget
    {
    Q_OBJECT

    public:
        explicit property_ui(const property_t& prop, Editable* obj, QWidget *parent = nullptr);

        ~property_ui() override;

    private:
        Ui::property_ui *ui;
        property_t _prop;

        Editable* _obj;

        std::variant<QLineEdit*, QCheckBox*> _iw;
    };
} // DesEngine

#endif //DESENGINE_PROPERTY_UI_HPP
