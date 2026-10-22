// AudioEngine.cpp
// Implementation of the core audio processing engine

#include "AudioEngine.h"
#include "AudioDeviceManager.h"
#include "PluginHost.h"
#include "MIDIController.h"

AudioEngine::AudioEngine() : isRunning(false) {
    deviceManager = std::make_unique<AudioDeviceManager>();
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::initialize() {
    // Initialize audio device manager
    if (!deviceManager->initialize()) {
        return false;
    }

    // Initialize plugins
    for (auto& plugin : plugins) {
        if (!plugin->initialize()) {
            return false;
        }
    }

    return true;
}

void AudioEngine::start() {
    if (!isRunning) {
        deviceManager->start();
        isRunning = true;
    }
}

void AudioEngine::stop() {
    if (isRunning) {
        deviceManager->stop();
        isRunning = false;
    }
}

void AudioEngine::processAudio(float** inputs, float** outputs, int numSamples) {
    // Process audio through plugins
    for (auto& plugin : plugins) {
        plugin->processAudio(inputs, outputs, numSamples);
    }

    // Handle MIDI input if controller is set
    if (midiController) {
        midiController->processMIDI();
    }
}

void AudioEngine::addPlugin(std::shared_ptr<PluginHost> plugin) {
    plugins.push_back(plugin);
}

void AudioEngine::removePlugin(std::shared_ptr<PluginHost> plugin) {
    plugins.erase(
        std::remove(plugins.begin(), plugins.end(), plugin),
        plugins.end()
    );
}

void AudioEngine::setMIDIController(std::shared_ptr<MIDIController> controller) {
    midiController = controller;
}

std::vector<std::string> AudioEngine::getAvailableDevices() {
    return deviceManager->getAvailableDevices();
}

bool AudioEngine::setActiveDevice(const std::string& deviceName) {
    return deviceManager->setActiveDevice(deviceName);
}