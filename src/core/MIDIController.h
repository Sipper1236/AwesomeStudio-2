// MIDIController.h
// MIDI Control Protocol (MCP) support for Awesome Studio

#pragma once

#include <string>
#include <vector>
#include <map>

struct MIDIMessage {
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    double timestamp;
};

struct ControllerMapping {
    int controllerNumber;
    std::string parameterPath;
    float minValue;
    float maxValue;
    bool isRelative;
};

class MIDIController {
public:
    MIDIController();
    virtual ~MIDIController();

    // Initialize the MIDI controller
    virtual bool initialize();

    // Start MIDI input
    virtual void start();

    // Stop MIDI input
    virtual void stop();

    // Process incoming MIDI messages
    virtual void processMIDI();

    // Add a controller mapping
    void addMapping(int controllerNumber, const std::string& parameterPath, float minValue, float maxValue, bool isRelative = false);

    // Remove a controller mapping
    void removeMapping(int controllerNumber);

    // Get all mappings
    const std::map<int, ControllerMapping>& getMappings() const;

    // Set parameter value from MIDI
    void setParameterFromMIDI(int controllerNumber, int value);

    // Get available MIDI input ports
    virtual std::vector<std::string> getAvailableInputPorts() = 0;

    // Set active MIDI input port
    virtual bool setActiveInputPort(const std::string& portName) = 0;

protected:
    std::map<int, ControllerMapping> mappings;
    bool isRunning;
};

// Concrete implementation for different platforms
class WindowsMIDIController : public MIDIController {
public:
    WindowsMIDIController();
    ~WindowsMIDIController() override;

    std::vector<std::string> getAvailableInputPorts() override;
    bool setActiveInputPort(const std::string& portName) override;
    bool initialize() override;
    void start() override;
    void stop() override;
};

class LinuxMIDIController : public MIDIController {
public:
    LinuxMIDIController();
    ~LinuxMIDIController() override;

    std::vector<std::string> getAvailableInputPorts() override;
    bool setActiveInputPort(const std::string& portName) override;
    bool initialize() override;
    void start() override;
    void stop() override;
};

class MacMIDIController : public MIDIController {
public:
    MacMIDIController();
    ~MacMIDIController() override;

    std::vector<std::string> getAvailableInputPorts() override;
    bool setActiveInputPort(const std::string& portName) override;
    bool initialize() override;
    void start() override;
    void stop() override;
};