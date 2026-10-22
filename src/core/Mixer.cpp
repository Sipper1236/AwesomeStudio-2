// Mixer.cpp
// Implementation of the mixer module

#include "Mixer.h"
#include <juce_processor_utils.h>

//==============================================================================
Mixer::Mixer() : sampleRate(44100.0), blockSize(512)
{
    // Initialize default mixer configuration
    // Main stereo output bus
    addBus("Master", juce::AudioChannelSet::stereo());

    // Main input channel
    addChannel("Input 1", juce::AudioChannelSet::stereo());
}

//==============================================================================
Mixer::~Mixer()
{
}

//==============================================================================
int Mixer::addChannel(const juce::String& name, juce::AudioChannelSet channelSet)
{
    MixerChannel channel;
    channel.id = channels.size();
    channel.channelSet = channelSet;
    channel.volume = 0.0f; // -inf dB (muted)
    channel.pan = 0.0f;    // Center
    channel.mute = false;
    channel.solo = false;
    channel.recordEnable = false;
    channel.name = name;
    channel.sends.resize(buses.size(), 0.0f); // Initialize sends to 0

    channels.push_back(channel);
    return channel.id;
}

//==============================================================================
void Mixer::removeChannel(int channelId)
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        channels.erase(channels.begin() + channelId);
        // Renumber remaining channels
        for (size_t i = 0; i < channels.size(); ++i) {
            channels[i].id = i;
        }
    }
}

//==============================================================================
MixerChannel& Mixer::getChannel(int channelId)
{
    jassert(channelId >= 0 && channelId < (int)channels.size());
    return channels[channelId];
}

//==============================================================================
const std::vector<MixerChannel>& Mixer::getChannels() const
{
    return channels;
}

//==============================================================================
int Mixer::addBus(const juce::String& name, juce::AudioChannelSet channelSet)
{
    MixerBus bus;
    bus.id = buses.size();
    bus.channelSet = channelSet;
    bus.name = name;
    bus.inputs.clear();

    buses.push_back(bus);
    // Add send slot to all existing channels
    for (auto& channel : channels) {
        channel.sends.push_back(0.0f);
    }

    return buses.size() - 1;
}

//==============================================================================
void Mixer::removeBus(int busId)
{
    if (busId >= 0 && busId < (int)buses.size()) {
        buses.erase(buses.begin() + busId);
        // Remove send slot from all channels
        for (auto& channel : channels) {
            if (busId < (int)channel.sends.size()) {
                channel.sends.erase(channel.sends.begin() + busId);
            }
        }
    }
}

//==============================================================================
MixerBus& Mixer::getBus(int busId)
{
    jassert(busId >= 0 && busId < (int)buses.size());
    return buses[busId];
}

//==============================================================================
const std::vector<MixerBus>& Mixer::getBuses() const
{
    return buses;
}

//==============================================================================
void Mixer::setChannelVolume(int channelId, float volume)
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        // Convert linear gain to dB range
        channels[channelId].volume = jlimit(-60.0f, 24.0f, volume);
    }
}

//==============================================================================
float Mixer::getChannelVolume(int channelId) const
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        return channels[channelId].volume;
    }
    return 0.0f;
}

//==============================================================================
void Mixer::setChannelPan(int channelId, float pan)
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        channels[channelId].pan = jlimit(-1.0f, 1.0f, pan);
    }
}

//==============================================================================
float Mixer::getChannelPan(int channelId) const
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        return channels[channelId].pan;
    }
    return 0.0f;
}

//==============================================================================
void Mixer::setChannelMute(int channelId, bool mute)
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        channels[channelId].mute = mute;
    }
}

//==============================================================================
bool Mixer::getChannelMute(int channelId) const
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        return channels[channelId].mute;
    }
    return false;
}

//==============================================================================
void Mixer::setChannelSolo(int channelId, bool solo)
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        channels[channelId].solo = solo;
    }
}

//==============================================================================
bool Mixer::getChannelSolo(int channelId) const
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        return channels[channelId].solo;
    }
    return false;
}

//==============================================================================
void Mixer::setChannelRecordEnable(int channelId, bool enable)
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        channels[channelId].recordEnable = enable;
    }
}

//==============================================================================
bool Mixer::getChannelRecordEnable(int channelId) const
{
    if (channelId >= 0 && channelId < (int)channels.size()) {
        return channels[channelId].recordEnable;
    }
    return false;
}

//==============================================================================
void Mixer::setSendLevel(int channelId, int busId, float level)
{
    if (channelId >= 0 && channelId < (int)channels.size() &&
        busId >= 0 && busId < (int)channels[channelId].sends.size()) {
        channels[channelId].sends[busId] = jlimit(0.0f, 1.0f, level);
    }
}

//==============================================================================
float Mixer::getSendLevel(int channelId, int busId) const
{
    if (channelId >= 0 && channelId < (int)channels.size() &&
        busId >= 0 && busId < (int)channels[channelId].sends.size()) {
        return channels[channelId].sends[busId];
    }
    return 0.0f;
}

//==============================================================================
void Mixer::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Get total number of channels
    int totalOutputChannels = jmax(buffer.getNumChannels(), 2);

    // Clear the output buffer
    buffer.clear();

    // Process each channel
    for (int channel = 0; channel < totalOutputChannels; ++channel) {
        // Apply channel volume and pan
        float channelVolume = 1.0f;
        float channelPan = 0.0f;

        // Check if this is a real output channel
        if (channel < (int)buffer.getNumChannels()) {
            // Find the corresponding mixer channel
            // For simplicity, we'll use channel index directly
            if (channel < (int)channels.size()) {
                channelVolume = channels[channel].volume > -60.0f ? juce::Decibels::gain(channels[channel].volume) : 0.0f;
                channelPan = channels[channel].pan;

                // Apply mute
                if (channels[channel].mute) {
                    channelVolume = 0.0f;
                }

                // Apply solo - solo channels mute all non-solo channels
                bool isSolo = channels[channel].solo;
                // In a real implementation, we'd check if any channel is in solo mode
            }
        }

        // Process the channel signal
        if (channel < (int)buffer.getNumChannels()) {
            auto* channelData = buffer.getWritePointer(channel);

            // Apply simple gain and pan
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                // Simple panning law
                float panLaw = 1.0f; // Simple linear panning
                float leftGain = jcos((juce::MathConstants<float>::halfPi) * (1.0f - channelPan));
                float rightGain = jcos((juce::MathConstants<float>::halfPi) * (1.0f + channelPan));

                // Apply volume
                float sampleValue = channelData[sample] * channelVolume;

                // Apply panning
                if (channel == 0) { // Left channel
                    channelData[sample] = sampleValue * leftGain;
                }
                else if (channel == 1) { // Right channel
                    channelData[sample] = sampleValue * rightGain;
                }
            }
        }
    }

    // Set block size for future reference
    this->blockSize = buffer.getNumSamples();
}

//==============================================================================
void Mixer::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = sampleRate;
    this->blockSize = samplesPerBlock;

    // Prepare any processors here
    // For mixer, no special preparation needed
}

//==============================================================================
void Mixer::releaseResources()
{
    // Release any cached resources
    // For mixer, no special cleanup needed
}

//==============================================================================
juce::AudioProcessorEditor* Mixer::createEditor()
{
    // Return a custom editor UI
    // For now, return null - would need a custom mixer editor
    return nullptr;
}