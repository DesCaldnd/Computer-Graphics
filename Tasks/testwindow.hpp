//
// Created by Des Caldnd on 10/17/2024.
//

#ifndef DESENGINE_TESTWINDOW_HPP
#define DESENGINE_TESTWINDOW_HPP

#include <QMainWindow>


QT_BEGIN_NAMESPACE
namespace Ui
{
    class TestWindow;
}
QT_END_NAMESPACE

class TestWindow : public QMainWindow
{
Q_OBJECT

public:
    explicit TestWindow(QWidget *parent = nullptr);

    ~TestWindow() override;

private:
    Ui::TestWindow *ui;
};


#endif //DESENGINE_TESTWINDOW_HPP
