// Mixer.h
// Mixer module for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <map>
#include <string>

struct MixerChannel
{
    int id;
    juce::AudioChannelSet channelSet;
    float volume;       // -inf to 6dB gain
    float pan;          // -1.0 (left) to 1.0 (right)
    bool mute;
    bool solo;
    bool recordEnable;
    std::string name;
    std::vector<float> sends; // Send levels to buses
};

struct MixerBus
{
    int id;
    juce::AudioChannelSet channelSet;
    std::string name;
    std::vector<std::pair<int, float>> inputs; // (channelIndex, level)
};

class Mixer : public juce::AudioProcessor
{
public:
    Mixer();
    ~Mixer() override;

    // Basic AudioProcessor interface
    const juce::String getName() const override { return "Mixer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Parameter handling
    int getNumParameters() const override { return 0; }
    float getParameter(int index) const override { return 0.0f; }
    void setParameter(int index, float value) override {}
    const juce::String getParameterName(int index) const override { return {}; }

    // Audio block processing
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    // Prepare to play
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    // Release resources
    void releaseResources() override;

    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool editorIsChanging() const override { return false; }

    // Add a channel to the mixer
    int addChannel(const juce::String& name, juce::AudioChannelSet channelSet);

    // Remove a channel
    void removeChannel(int channelId);

    // Get a channel
    MixerChannel& getChannel(int channelId);

    // Get all channels
    const std::vector<MixerChannel>& getChannels() const;

    // Add a bus
    int addBus(const juce::String& name, juce::AudioChannelSet channelSet);

    // Remove a bus
    void removeBus(int busId);

    // Get a bus
    MixerBus& getBus(int busId);

    // Get all buses
    const std::vector<MixerBus>& getBuses() const;

    // Set channel volume
    void setChannelVolume(int channelId, float volume);

    // Get channel volume
    float getChannelVolume(int channelId) const;

    // Set channel pan
    void setChannelPan(int channelId, float pan);

    // Get channel pan
    float getChannelPan(int channelId) const;

    // Set channel mute
    void setChannelMute(int channelId, bool mute);

    // Get channel mute
    bool getChannelMute(int channelId) const;

    // Set channel solo
    void setChannelSolo(int channelId, bool solo);

    // Get channel solo
    bool getChannelSolo(int channelId) const;

    // Set channel record enable
    void setChannelRecordEnable(int channelId, bool enable);

    // Get channel record enable
    bool getChannelRecordEnable(int channelId) const;

    // Set send level
    void setSendLevel(int channelId, int busId, float level);

    // Get send level
    float getSendLevel(int channelId, int busId) const;

private:
    std::vector<MixerChannel> channels;
    std::vector<MixerBus> buses;
    double sampleRate;
    int blockSize;
};