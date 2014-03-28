#include "gui_main_window.h"
#include "qt_utils/exception_handling_application.h"

int main(int argc, char *argv[])
{
    qu::ExceptionHandlingApplication a(argc, argv);
    // Set up global organization name and application name, so
    // the default constructor of QSettings will use these
    // consistently throughout the application.
    QCoreApplication::setOrganizationName("Ralph Tandetzky");
    QCoreApplication::setApplicationName("Interactive IMF Decomposition");
    gui::MainWindow w;
    w.show();
    
    return a.exec();
}
