//#include <QtPlugin>
//
//Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)

#include <QApplication>
#include <Widgets/glmainwindow.hpp>
#include <Widgets/glwidget.hpp>
#include "include/Bezier3.hpp"

int main(int argc, char *argv[])
{
	QApplication a(argc, argv);

    QSurfaceFormat format;
    format.setSamples(16);
//    format.setSwapBehavior(QSurfaceFormat::SwapBehavior::SingleBuffer);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);

    DesEngine::GLMainWindow window;
    window.show();

    window.scene.add_object_loader("Bezier3", std::make_pair(&Bezier3::default_bezier3_object_json_loader, &Bezier3::default_bezier3_object_dialog_loader));

    window.scene.init();
    window.scene.init_in_edit_mode();

	return QApplication::exec();
}
