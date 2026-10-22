// PluginProcessor.cpp
// Implementation of the plugin processor

#include "PluginProcessor.h"

//==============================================================================
PluginProcessor::PluginProcessor()
{
}

//==============================================================================
PluginProcessor::~PluginProcessor()
{
}

//==============================================================================
void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Clear the buffer to silence by default
    buffer.clear();

    // Process any MIDI messages if needed
    // for (const auto midiMsg : midiMessages)
    // {
    //     // Handle MIDI input
    // }
}

//==============================================================================
void PluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Prepare any processors here
    juce::ignoreUnused(sampleRate, samplesPerBlock);
}

//==============================================================================
void PluginProcessor::releaseResources()
{
    // Release any cached resources
}

//==============================================================================
juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    // Return a custom editor UI
    // return new juce::GenericEditor(this);
    return nullptr;
}