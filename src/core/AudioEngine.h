// AudioEngine.h
// Core audio processing engine for Awesome Studio

#pragma once

#include <vector>
#include <memory>
#include <string>

class AudioDeviceManager;
class PluginHost;
class MIDIController;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Initialize the audio engine
    bool initialize();

    // Start audio processing
    void start();

    // Stop audio processing
    void stop();

    // Process audio
    void processAudio(float** inputs, float** outputs, int numSamples);

    // Add a plugin to the engine
    void addPlugin(std::shared_ptr<PluginHost> plugin);

    // Remove a plugin from the engine
    void removePlugin(std::shared_ptr<PluginHost> plugin);

    // Set the MIDI controller
    void setMIDIController(std::shared_ptr<MIDIController> controller);

    // Get available audio devices
    std::vector<std::string> getAvailableDevices();

    // Set the active audio device
    bool setActiveDevice(const std::string& deviceName);

private:
    std::unique_ptr<AudioDeviceManager> deviceManager;
    std::vector<std::shared_ptr<PluginHost>> plugins;
    std::shared_ptr<MIDIController> midiController;
    bool isRunning;
};
