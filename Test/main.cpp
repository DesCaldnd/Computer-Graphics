//#include <QtPlugin>
//
//Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)

#include <QApplication>
#include <Widgets/glmainwindow.hpp>
#include <Widgets/glwidget.hpp>

int main(int argc, char *argv[])
{
	QApplication a(argc, argv);

    DesEngine::GLMainWindow window;
    window.show();

    window.scene.init();
    window.scene.init_in_edit_mode();

	return QApplication::exec();
}
