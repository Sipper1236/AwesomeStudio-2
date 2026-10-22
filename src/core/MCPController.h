// MCPController.h
// MIDI Control Protocol (MCP) implementation for Awesome Studio

#pragma once

#include <JuceHeader.h>
#include <map>
#include <string>

// MCP Message types
enum class MCPMessageType {
    MasterVolume,
    Solo,
    Mute,
    RecordEnable,
    Select,
    armForRecording,
    stop,
    play,
    record,
    stopAll,
    toggleRecord,
    cycle,
    locate,
    markers,
    timecode,
    transport,
    mixer,
    plugin,
    track,
    bus,
    send,
    folder,
    group,
    bussubmix,
    output,
    input,
    channelstrip,
    mixerstrip,
    aux,
    master,
    meta,
    syscommon,
    sysex
};

// MCP Message structure
struct MCPMessage {
    MCPMessageType type;
    int value;
    double timestamp;
    std::string target;
    std::string detail;
};

// MCP Controller class - handles MCP over MIDI
class MCPController : public juce::MidiInputCallback
{
public:
    MCPController();
    ~MCPController() override;

    // Initialize MCP controller
    bool initialize();

    // Start listening for MCP messages
    void start();

    // Stop listening
    void stop();

    // Get all received messages
    std::vector<MCPMessage> getMessages() const;

    // Clear messages
    void clearMessages();

    // MidiInputCallback implementation
    void handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message) override;

    // Process an MCP message
    void processMessage(const MidiMessage& message);

    // Send an MCP message (for output)
    void sendMessage(MCPMessageType type, int value = 0, const std::string& target = "", const std::string& detail = "");

    // Get available MIDI inputs
    std::vector<std::string> getAvailableInputs();

    // Set active MIDI input
    bool setActiveInput(const std::string& portName);

    // Subscribe to a specific track/channel
    void subscribeTrack(int trackNumber);

    // Unsubscribe from a track
    void unsubscribeTrack(int trackNumber);

    // Subscribe to a channel
    void subscribeChannel(int channel);

    // Unsubscribe from a channel
    void unsubscribeChannel(int channel);

    // Get current track
    int getCurrentTrack() const;

    // Set current track
    void setCurrentTrack(int trackNumber);

    // Get current channel
    int getCurrentChannel() const;

    // Set current channel
    void setCurrentChannel(int channelNumber);

    // Master volume control
    void setMasterVolume(float volume);

    // Get master volume
    float getMasterVolume() const;

    // Solo state
    void setSoloState(int track, bool solo);
    bool getSoloState(int track) const;

    // Mute state
    void setMuteState(int track, bool muted);
    bool getMuteState(int track) const;

    // Record enable state
    void setRecordEnableState(int track, bool enabled);
    bool getRecordEnableState(int track) const;

    // Track selection
    void setTrackSelected(int track, bool selected);
    bool isTrackSelected(int track) const;

    // Get track name
    std::string getTrackName(int track) const;

    // Get track number
    int getTrackCount() const;

private:
    std::vector<MCPMessage> messages;
    bool isListening;
    int currentTrack;
    int currentChannel;
    float masterVolume;

    // Track states
    std::map<int, bool> soloStates;
    std::map<int, bool> muteStates;
    std::map<int, bool> recordEnableStates;
    std::map<int, bool> trackSelectedStates;
    std::map<int, std::string> trackNames;
    int trackCount;
};