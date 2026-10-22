// PluginHost.h
// Base class for plugin hosting in Awesome Studio

#pragma once

#include <string>
#include <vector>

class PluginHost {
public:
    virtual ~PluginHost() = default;

    // Plugin identification
    virtual std::string getName() const = 0;
    virtual std::string getVersion() const = 0;
    virtual std::string getUniqueID() const = 0;

    // Plugin lifecycle
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;

    // Audio processing
    virtual void processAudio(float** inputs, float** outputs, int numSamples) = 0;

    // MIDI processing
    virtual void processMIDI(const unsigned char* data, int length) = 0;

    // Parameter handling
    virtual int getParameterCount() const = 0;
    virtual std::string getParameterName(int index) const = 0;
    virtual float getParameterValue(int index) const = 0;
    virtual void setParameterValue(int index, float value) = 0;

    // State management
    virtual void getStateInformation(std::vector<char>& data) = 0;
    virtual void setStateInformation(const std::vector<char>& data) = 0;

    // Plugin format
    virtual std::string getFormat() const = 0;
};