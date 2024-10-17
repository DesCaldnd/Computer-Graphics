//
// Created by Des Caldnd on 10/17/2024.
//

#ifndef DESENGINE_PROPERTYWIDGET_HPP
#define DESENGINE_PROPERTYWIDGET_HPP

#include <QWidget>

namespace DesEngine
{
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
        explicit PropertyWidget(QWidget *parent = nullptr);

        ~PropertyWidget() override;

    private:
        Ui::PropertyWidget *ui;
    };
} // DesEngine

#endif //DESENGINE_PROPERTYWIDGET_HPP
