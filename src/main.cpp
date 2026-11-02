// =============================================================================
//  MD Studio - entry point.
//  Command line:  MDStudio [file-or-folder ...]
// =============================================================================
#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("MDStudio");
    QCoreApplication::setApplicationName("MDStudio");
    QCoreApplication::setApplicationVersion(MDSTUDIO_VERSION);

    // Window / taskbar icon (the .exe icon itself is embedded by CMake, see CMakeLists.txt).
    QIcon appIcon;
    for (int sz : {16, 24, 32, 48, 64, 128, 256, 512})
        appIcon.addFile(QStringLiteral(":/icons/mdstudio-%1.png").arg(sz), QSize(sz, sz));
    app.setWindowIcon(appIcon);

    MainWindow window;
    window.show();

    // Files / folders passed on the command line (also "Open with..." on Windows).
    const QStringList args = app.arguments().mid(1);
    for (const QString &a : args) {
        const QFileInfo fi(a);
        if (fi.isDir())         window.openFolder(fi.absoluteFilePath());
        else if (fi.exists())   window.openFile(fi.absoluteFilePath());
    }

    // Developer hook: MDSTUDIO_SHOT=out.png grabs the window after a moment and quits.
    if (const QByteArray shot = qgetenv("MDSTUDIO_SHOT"); !shot.isEmpty()) {
        QTimer::singleShot(1500, &window, [&window, shot, &app] {
            window.grab().save(QString::fromLocal8Bit(shot));
            app.quit();
        });
    }
    return app.exec();
}
