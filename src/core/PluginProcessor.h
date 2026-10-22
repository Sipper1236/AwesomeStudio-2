// PluginProcessor.h
// Base class for plugin processing in Awesome Studio

#pragma once

#include <JuceHeader.h>

class PluginProcessor : public juce::AudioPluginInstance
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

    // Basic AudioPluginInstance interface
    const juce::String getName() const override { return "AwesomePlugin"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Parameter handling
    int getNumParameters() const override { return 0; }
    float getParameter(int index) const override { return 0.0f; }
    void setParameter(int index, float value) override {}
    const juce::String getParameterName(int index) const override { return {}; }

    // Audio processing
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    // Prepare to play
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    // Release resources
    void releaseResources() override;

    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool editorIsChanging() const override { return false; }

    // State
    juce::UndoManager* getUndoManager() override { return nullptr; }
};