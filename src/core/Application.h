// Application.h
// Main application class for Awesome Studio

#pragma once

#include <string>
#include <memory>

#include "AudioEngine.h"
#include "PluginHost.h"
#include "MIDIController.h"

class QApplication;
class MainWindow;

class Application {
public:
    Application();
    ~Application();

    // Initialize the application
    bool initialize(int argc, char* argv[]);

    // Run the application main loop
    int run();

    // Shutdown the application
    void shutdown();

    // Get the audio engine
    AudioEngine& getAudioEngine();

    // Get the main window
    MainWindow* getMainWindow();

    // Load a project
    bool loadProject(const std::string& projectPath);

    // Save the current project
    bool saveProject(const std::string& projectPath);

    // Create a new project
    bool newProject();

private:
    std::unique_ptr<AudioEngine> audioEngine;
    std::unique_ptr<QApplication> qtApplication;
    MainWindow* mainWindow;
    bool isRunning;
};