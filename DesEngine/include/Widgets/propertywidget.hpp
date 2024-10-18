//
// Created by Des Caldnd on 10/17/2024.
//

#ifndef DESENGINE_PROPERTYWIDGET_HPP
#define DESENGINE_PROPERTYWIDGET_HPP

#include <QWidget>
#include "Interfaces//Serializable.hpp"

namespace DesEngine
{
    class Scene;
    class LogicObject;

    QT_BEGIN_NAMESPACE
    namespace Ui
    {
        class PropertyWidget;
    }
    QT_END_NAMESPACE

    class PropertyWidget : public QWidget
    {
    Q_OBJECT

    public:
        explicit PropertyWidget(Scene* scene, QWidget *parent = nullptr);

        ~PropertyWidget() override;

        void select_object(std::shared_ptr<LogicObject> obj);
        void unselect_object();

        void change_camera(id_t id);

    private:
        Ui::PropertyWidget *ui;

        LogicObject* _obj = nullptr;
    };
} // DesEngine

#endif //DESENGINE_PROPERTYWIDGET_HPP
