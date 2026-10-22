// MainWindow.h
// Main window UI for Awesome Studio

#pragma once

#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QDockWidget>
#include <QTabWidget>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onPreferences();
    void onAbout();

private:
    void createActions();
    void createMenus();
    void createToolBars();
    void createDockWidgets();
    void setupConnections();

    QMenuBar* menuBar;
    QToolBar* toolBar;
    QTabWidget* centralWidget;

    // Menus
    QMenu* fileMenu;
    QMenu* editMenu;
    QMenu* viewMenu;
    QMenu* projectMenu;
    QMenu* pluginsMenu;
    QMenu* toolsMenu;
    QMenu* helpMenu;

    // Actions
    QAction* newProjectAction;
    QAction* openProjectAction;
    QAction* saveProjectAction;
    QAction* saveProjectAsAction;
    QAction* preferencesAction;
    QAction* aboutAction;
    QAction* exitAction;

    // Dock widgets
    QDockWidget* mixerDock;
    QDockWidget* browserDock;
    QDockWidget* inspectorDock;
    QDockWidget* transportDock;
    QDockWidget* midiDock;
};