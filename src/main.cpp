#include "core/AudioEngine.h"
#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Awesome Studio"));
    application.setApplicationVersion(QStringLiteral("0.2.0"));
    application.setOrganizationName(QStringLiteral("Awesome Studio"));

    AudioEngine audioEngine;
    MainWindow mainWindow(audioEngine);
    mainWindow.show();
    return application.exec();
}
