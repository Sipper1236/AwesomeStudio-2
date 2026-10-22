// MainWindow.cpp
// Implementation of the main window UI

#include "MainWindow.h"
#include <QAction>
#include <QMenu>
#include <QToolBar>
#include <QDockWidget>
#include <QTabWidget>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Awesome Studio");
    resize(1200, 800);

    createActions();
    createMenus();
    createToolBars();
    createDockWidgets();
    setupConnections();

    // Set central widget
    centralWidget = new QTabWidget(this);
    setCentralWidget(centralWidget);

    // Restore settings
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}

MainWindow::~MainWindow() {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
}

void MainWindow::createActions() {
    newProjectAction = new QAction(tr("&New Project"), this);
    newProjectAction->setShortcut(QKeySequence::New);
    newProjectAction->setStatusTip(tr("Create a new project"));

    openProjectAction = new QAction(tr("&Open Project..."), this);
    openProjectAction->setShortcut(QKeySequence::Open);
    openProjectAction->setStatusTip(tr("Open an existing project"));

    saveProjectAction = new QAction(tr("&Save Project"), this);
    saveProjectAction->setShortcut(QKeySequence::Save);
    saveProjectAction->setStatusTip(tr("Save the current project"));

    saveProjectAsAction = new QAction(tr("Save Project &As..."), this);
    saveProjectAsAction->setShortcut(QKeySequence::SaveAs);
    saveProjectAsAction->setStatusTip(tr("Save the project with a new name"));

    preferencesAction = new QAction(tr("&Preferences..."), this);
    preferencesAction->setShortcut(QKeySequence::Preferences);
    preferencesAction->setStatusTip(tr("Open preferences dialog"));

    aboutAction = new QAction(tr("&About"), this);
    aboutAction->setStatusTip(tr("Show about dialog"));

    exitAction = new QAction(tr("E&xit"), this);
    exitAction->setShortcut(QKeySequence::Quit);
    exitAction->setStatusTip(tr("Exit the application"));
}

void MainWindow::createMenus() {
    fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(newProjectAction);
    fileMenu->addAction(openProjectAction);
    fileMenu->addSeparator();
    fileMenu->addAction(saveProjectAction);
    fileMenu->addAction(saveProjectAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(preferencesAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exitAction);

    editMenu = menuBar()->addMenu(tr("&Edit"));

    viewMenu = menuBar()->addMenu(tr("&View"));

    projectMenu = menuBar()->addMenu(tr("&Project"));

    pluginsMenu = menuBar()->addMenu(tr("&Plugins"));

    toolsMenu = menuBar()->addMenu(tr("&Tools"));

    helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(aboutAction);
}

void MainWindow::createToolBars() {
    toolBar = addToolBar(tr("Main Toolbar"));
    toolBar->addAction(newProjectAction);
    toolBar->addAction(openProjectAction);
    toolBar->addAction(saveProjectAction);
    toolBar->addSeparator();
}

void MainWindow::createDockWidgets() {
    mixerDock = new QDockWidget(tr("Mixer"), this);
    mixerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, mixerDock);

    browserDock = new QDockWidget(tr("Browser"), this);
    browserDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);

    inspectorDock = new QDockWidget(tr("Inspector"), this);
    inspectorDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);

    transportDock = new QDockWidget(tr("Transport"), this);
    transportDock->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, transportDock);

    midiDock = new QDockWidget(tr("MIDI Editor"), this);
    midiDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    // Set the MIDI editor as the central widget of the dock
    MidiEditor* midiEditor = new MidiEditor();
    midiDock->setWidget(midiEditor);
    addDockWidget(Qt::LeftDockWidgetArea, midiDock);
}

void MainWindow::setupConnections() {
    connect(newProjectAction, &QAction::triggered, this, &MainWindow::onNewProject);
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::onOpenProject);
    connect(saveProjectAction, &QAction::triggered, this, &MainWindow::onSaveProject);
    connect(saveProjectAsAction, &QAction::triggered, this, &MainWindow::onSaveProjectAs);
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::onPreferences);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::onNewProject() {
    // Implementation for new project
}

void MainWindow::onOpenProject() {
    QString fileName = QFileDialog::getOpenFileName(this, tr("Open Project"), "", tr("Awesome Studio Projects (*.aws)"));
    if (!fileName.isEmpty()) {
        // Load project
    }
}

void MainWindow::onSaveProject() {
    // Implementation for save project
}

void MainWindow::onSaveProjectAs() {
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save Project As"), "", tr("Awesome Studio Projects (*.aws)"));
    if (!fileName.isEmpty()) {
        // Save project
    }
}

void MainWindow::onPreferences() {
    // Implementation for preferences dialog
}

void MainWindow::onAbout() {
    QMessageBox::about(this, tr("About Awesome Studio"),
        tr("Awesome Studio v1.0.0\n"
           "A digital audio workstation with MCP support\n\n"
           "Copyright 2026 Awesome Studio Team"));
}