#include <QApplication>
#include <QFile>
#include <QIcon>

#include <pylon/PylonIncludes.h>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    // One init for the whole process; Recorder and MainWindow both use pylon.
    Pylon::PylonAutoInitTerm pylonInit;

    QApplication app(argc, argv);
    app.setApplicationName("RatCam Recorder");
    app.setOrganizationName("RatCam");

    // Window and taskbar icon. Compiled in when the artwork exists; absent is
    // not an error, the app simply uses the default.
    if (QFile::exists(":/ratcam.png")) app.setWindowIcon(QIcon(":/ratcam.png"));

    campy::MainWindow window;
    window.show();
    return app.exec();
}
