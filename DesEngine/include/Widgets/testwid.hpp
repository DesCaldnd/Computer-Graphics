//
// Created by Des Caldnd on 10/17/2024.
//

#ifndef DESENGINE_TESTWID_HPP
#define DESENGINE_TESTWID_HPP

#include <QWidget>

namespace DesEngine
{
    QT_BEGIN_NAMESPACE
    namespace Ui
    {
        class testwid;
    }
    QT_END_NAMESPACE

    class testwid : public QWidget
    {
    Q_OBJECT

    public:
        explicit testwid(QWidget *parent = nullptr);

        ~testwid() override;

    private:
        Ui::testwid *ui;
    };
} // DesEngine

#endif //DESENGINE_TESTWID_HPP
