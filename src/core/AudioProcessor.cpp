// AudioProcessor.cpp
// Implementation of the audio processor

#include "AudioProcessor.h"

//==============================================================================
AudioProcessor::AudioProcessor()
{
}

//==============================================================================
AudioProcessor::~AudioProcessor()
{
}

//==============================================================================
void AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Clear the buffer to silence
    buffer.clear();

    // Process MIDI messages if any
    // for (const auto midiMessageInContext : midiMessages)
    // {
    //     // Handle MIDI input
    // }
}

//==============================================================================
void AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = sampleRate;
    this->blockSize = samplesPerBlock;

    // Prepare any processors here
}

//==============================================================================
void AudioProcessor::releaseResources()
{
    // Release any cached resources
}

//==============================================================================
juce::AudioProcessorEditor* AudioProcessor::createEditor()
{
    // Return a custom editor UI
    // return new juce::GenericEditor(this);
    return nullptr;
}