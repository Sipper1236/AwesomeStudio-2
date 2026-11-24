#include "core/AudioEngine.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Awesome Studio"));
    application.setApplicationVersion(QStringLiteral("0.2.0"));
    application.setOrganizationName(QStringLiteral("Awesome Studio"));

    AudioEngine audioEngine;
    MainWindow mainWindow(audioEngine);
    mainWindow.show();
    const QStringList arguments = application.arguments();
    if (arguments.size() > 1)
    {
        const QString projectPath = QFileInfo(arguments.at(1)).absoluteFilePath();
        QTimer::singleShot(0, &mainWindow, [&mainWindow, projectPath]
        {
            mainWindow.openProjectFile(projectPath);
        });
    }
    return application.exec();
}
