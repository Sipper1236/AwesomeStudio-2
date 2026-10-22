// Application.cpp
// Main application implementation for Awesome Studio

#include "Application.h"
#include "AudioEngine.h"
#include <iostream>

Application::Application() : isRunning(false) {
    audioEngine = std::make_unique<AudioEngine>();
}

Application::~Application() {
    shutdown();
}

bool Application::initialize() {
    std::cout << "Initializing Awesome Studio..." << std::endl;

    // Initialize audio engine
    if (!audioEngine->initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return false;
    }

    // Initialize other subsystems
    // pluginManager = std::make_unique<PluginManager>();
    // midiControllerManager = std::make_unique<MIDIControllerManager>();

    std::cout << "Awesome Studio initialized successfully" << std::endl;
    return true;
}

int Application::run() {
    if (!isRunning) {
        isRunning = true;
        std::cout << "Starting Awesome Studio main loop..." << std::endl;

        // Start audio engine
        audioEngine->start();

        // Main application loop would go here
        // This would typically be a Qt event loop or similar

        // For now, just simulate running
        while (isRunning) {
            // Process events, update UI, etc.
            // This is a simplified version
        }

        std::cout << "Awesome Studio main loop ended" << std::endl;
    }
    return 0;
}

void Application::shutdown() {
    if (isRunning) {
        std::cout << "Shutting down Awesome Studio..." << std::endl;

        // Stop audio engine
        audioEngine->stop();

        // Shutdown other subsystems
        isRunning = false;

        std::cout << "Awesome Studio shutdown complete" << std::endl;
    }
}

AudioEngine& Application::getAudioEngine() {
    return *audioEngine;
}

bool Application::loadProject(const std::string& projectPath) {
    std::cout << "Loading project: " << projectPath << std::endl;
    // Implementation would load project data
    return true;
}

bool Application::saveProject(const std::string& projectPath) {
    std::cout << "Saving project: " << projectPath << std::endl;
    // Implementation would save project data
    return true;
}

bool Application::newProject() {
    std::cout << "Creating new project" << std::endl;
    // Implementation would create new project
    return true;
}