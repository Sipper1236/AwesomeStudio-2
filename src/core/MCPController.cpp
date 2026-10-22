// MCPController.cpp
// Implementation of the MCP controller

#include "MCPController.h"
#include <JuceHeader.h>

MCPController::MCPController()
    : isListening(false), currentTrack(-1), currentChannel(1), masterVolume(0.5f), trackCount(0)
{
}

//==============================================================================
MCPController::~MCPController()
{
    stop();
}

//==============================================================================
bool MCPController::initialize()
{
    // Get available MIDI inputs
    auto availableInputs = getAvailableInputs();

    if (availableInputs.empty()) {
        std::cout << "No MIDI inputs available" << std::endl;
        return false;
    }

    // Set first available input as active
    if (!setActiveInput(availableInputs[0])) {
        return false;
    }

    isListening = true;
    std::cout << "MCP Controller initialized with " << availableInputs.size() << " MIDI input(s)" << std::endl;
    return true;
}

//==============================================================================
void MCPController::start()
{
    if (!isListening) {
        if (!initialize()) {
            std::cerr << "Failed to initialize MCP controller" << std::endl;
            return;
        }
    }

    std::cout << "MCP Controller started listening" << std::endl;
}

//==============================================================================
void MCPController::stop()
{
    isListening = false;
    messages.clear();
    std::cout << "MCP Controller stopped" << std::endl;
}

//==============================================================================
std::vector<MCPMessage> MCPController::getMessages() const
{
    return messages;
}

//==============================================================================
void MCPController::clearMessages()
{
    messages.clear();
}

//==============================================================================
void MCPController::handleIncomingMidiMessage(MidiInput* source, const MidiMessage& message)
{
    // Convert MIDI message to MCP message
    processMessage(message);
}

//==============================================================================
void MCPController::processMessage(const MidiMessage& message)
{
    MCPMessage mcpMsg;
    mcpMsg.timestamp = message.getTimestamp();

    // Parse MIDI message into MCP format
    // This is a simplified parser - real implementation would be more complex

    if (message.isNoteOn()) {
        // Note on - could be used for track selection or triggering
        mcpMsg.type = MCPMessageType::select;
        mcpMsg.value = message.getNoteNumber();
    }
    else if (message.isNoteOff()) {
        mcpMsg.type = MCPMessageType::stop;
        mcpMsg.value = message.getNoteNumber();
    }
    else if (message.isControllerControlChange()) {
        int controllerNum = message.getControllerNumber();
        int controllerValue = message.getControllerValue();

        // Common MCP controller mappings
        switch (controllerNum) {
            case 7:  // Main volume
                mcpMsg.type = MCPMessageType::MasterVolume;
                mcpMsg.value = controllerValue;
                break;
            case 69: // Solo (varies by controller)
            case 67: // Mute (varies by controller)
                // These would need controller-specific mapping
                break;
            case 65: // Portamento
                break;
            case 64: // Damper pedal
                break;
            default:
                // Generic controller
                mcpMsg.type = MCPMessageType::mixer;
                mcpMsg.value = controllerValue;
                break;
        }
    }
    else if (message.isPitchWheelChange()) {
        // Pitch wheel - could be used for parameter control
        mcpMsg.type = MCPMessageType::mixer;
        mcpMsg.value = message.getPitchWheel();
    }
    else if (message.isAftertouch()) {
        // Aftertouch - expression control
        mcpMsg.type = MCPMessageType::mixer;
        mcpMsg.value = message.getFloatValue();
    }

    // Add timestamp and store
    mcpMsg.timestamp = message.getTimestamp();
    messages.push_back(mcpMsg);
}

//==============================================================================
void MCPController::sendMessage(MCPMessageType type, int value, const std::string& target, const std::string& detail)
{
    // Create MCP message and send via MIDI
    MCPMessage msg;
    msg.type = type;
    msg.value = value;
    msg.target = target;
    msg.detail = detail;
    msg.timestamp = Time::getCurrentTime();

    // In a real implementation, this would encode and send via MIDI
    // For now, just store it
    messages.push_back(msg);

    std::cout << "MCP Message sent: " << static_cast<int>(type) << " value=" << value
              << " target=" << target << std::endl;
}

//==============================================================================
std::vector<std::string> MCPController::getAvailableInputs()
{
    // Get MIDI input devices
    std::vector<std::string> inputs;

    // Enumerate MIDI input devices
    // This would use Juce's MidiInput::getAvailableInputs() in real implementation

    // For now, return empty or mock data
    // inputs.push_back("MIDI Device 1");
    // inputs.push_back("MIDI Device 2");

    return inputs;
}

//==============================================================================
bool MCPController::setActiveInput(const std::string& portName)
{
    // Set the active MIDI input port
    // In real implementation would use Juce's MidiDeviceManager

    // For now, just log and return success
    std::cout << "Setting active MIDI input: " << portName << std::endl;
    return true;
}

//==============================================================================
void MCPController::subscribeTrack(int trackNumber)
{
    currentTrack = trackNumber;
    trackNames[trackNumber] = "Track " + std::to_string(trackNumber);
    std::cout << "Subscribed to track " << trackNumber << std::endl;
}

//==============================================================================
void MCPController::unsubscribeTrack(int trackNumber)
{
    trackNames.erase(trackNumber);
    std::cout << "Unsubscribed from track " << trackNumber << std::endl;
}

//==============================================================================
void MCPController::subscribeChannel(int channel)
{
    currentChannel = channel;
    std::cout << "Subscribed to channel " << channel << std::endl;
}

//==============================================================================
void MCPController::unsubscribeChannel(int channel)
{
    // No state to clean up for channels
}

//==============================================================================
int MCPController::getCurrentTrack() const
{
    return currentTrack;
}

//==============================================================================
void MCPController::setCurrentTrack(int trackNumber)
{
    currentTrack = trackNumber;
    std::cout << "Current track set to " << trackNumber << std::endl;
}

//==============================================================================
int MCPController::getCurrentChannel() const
{
    return currentChannel;
}

//==============================================================================
void MCPController::setCurrentChannel(int channelNumber)
{
    currentChannel = channelNumber;
    std::cout << "Current channel set to " << channelNumber << std::endl;
}

//==============================================================================
void MCPController::setMasterVolume(float volume)
{
    masterVolume = jlimit(0.0f, 1.0f, volume);
    std::cout << "Master volume set to " << masterVolume << std::endl;
}

//==============================================================================
float MCPController::getMasterVolume() const
{
    return masterVolume;
}

//==============================================================================
void MCPController::setSoloState(int track, bool solo)
{
    soloStates[track] = solo;
    std::cout << "Track " << track << " solo set to " << (solo ? "ON" : "OFF") << std::endl;
}

//==============================================================================
bool MCPController::getSoloState(int track) const
{
    auto it = soloStates.find(track);
    if (it != soloStates.end()) {
        return it->second;
    }
    return false;
}

//==============================================================================
void MCPController::setMuteState(int track, bool muted)
{
    muteStates[track] = muted;
    std::cout << "Track " << track << " mute set to " << (muted ? "ON" : "OFF") << std::endl;
}

//==============================================================================
bool MCPController::getMuteState(int track) const
{
    auto it = muteStates.find(track);
    if (it != muteStates.end()) {
        return it->second;
    }
    return false;
}

//==============================================================================
void MCPController::setRecordEnableState(int track, bool enabled)
{
    recordEnableStates[track] = enabled;
    std::cout << "Track " << track << " record enable set to " << (enabled ? "ON" : "OFF") << std::endl;
}

//==============================================================================
bool MCPController::getRecordEnableState(int track) const
{
    auto it = recordEnableStates.find(track);
    if (it != recordEnableStates.end()) {
        return it->second;
    }
    return false;
}

//==============================================================================
void MCPController::setTrackSelected(int track, bool selected)
{
    trackSelectedStates[track] = selected;
    std::cout << "Track " << track << " selection set to " << (selected ? "SELECTED" : "not selected") << std::endl;
}

//==============================================================================
bool MCPController::isTrackSelected(int track) const
{
    auto it = trackSelectedStates.find(track);
    if (it != trackSelectedStates.end()) {
        return it->second;
    }
    return false;
}

//==============================================================================
std::string MCPController::getTrackName(int track) const
{
    auto it = trackNames.find(track);
    if (it != trackNames.end()) {
        return it->second;
    }
    return "Unknown Track";
}

//==============================================================================
int MCPController::getTrackCount() const
{
    return trackCount;
}