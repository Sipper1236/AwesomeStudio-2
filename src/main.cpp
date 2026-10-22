// main.cpp
// Entry point for Awesome Studio

#include <QApplication>
#include "core/Application.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Awesome Studio");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("Awesome Studio Team");

    Application awesomeStudio;

    if (!awesomeStudio.initialize()) {
        return -1;
    }

    return awesomeStudio.run();
}