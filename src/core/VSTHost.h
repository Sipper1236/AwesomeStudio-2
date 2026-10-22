// VSTHost.h
// VST plugin hosting for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <map>
#include <string>
#include <vector>

// VST plugin wrapper class
class VSTPluginWrapper : public juce::AudioPluginInstance
{
public:
    VSTPluginWrapper();
    ~VSTPluginWrapper() override;

    // Basic AudioPluginInstance interface
    const juce::String getName() const override;
    bool acceptsMidi() const override { return midiInput; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return isMidiEffect; }

    // Parameter handling
    int getNumParameters() const override;
    float getParameter(int index) const override;
    void setParameter(int index, float value) override;
    const juce::String getParameterName(int index) const override;

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

    // Load a VST plugin from file
    bool loadPlugin(const juce::File& pluginFile);

    // Get plugin path
    juce::File getPluginPath() const;

    // Get plugin unique ID
    juce::String getUniqueID() const;

    // Check if plugin is valid
    bool isValid() const;

    // Get number of inputs/outputs
    int getNumInputs() const override;
    int getNumOutputs() const override;

private:
    // JUCE VST3 plug-in (or VST2 wrapper)
    std::unique_ptr<juce::VST3::Plugin> vstPlugin;

    // Plugin info
    juce::String pluginName;
    juce::String pluginPath;
    juce::String uniqueID;
    bool isInitialized;
    bool midiInput;
    bool isMidiEffect;

    // Audio spec
    int numInputs;
    int numOutputs;
    int maxPolyphony;
};