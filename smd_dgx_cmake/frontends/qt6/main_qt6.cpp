#include <QApplication>
#include <QTimer>
#include "MainWindow.h"

// global flags expected by gpgx frontend core
int joynum    = 0;
int log_error = 0;
int debug_on  = 0;
int turbo_mode= 0;
int use_sound = 1;
int fullscreen= 0;

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Genesis Plus GX Debugger"));

    MainWindow w;
    w.show();

    if (argc >= 2) {
        QString romPath = QString::fromLocal8Bit(argv[1]);
        QTimer::singleShot(0, &w, [&w, romPath]{ w.openRomFile(romPath); });
    } else {
        // ask for ROM on first launch
        QTimer::singleShot(0, &w, &MainWindow::openRom);
    }

    return app.exec();
}
