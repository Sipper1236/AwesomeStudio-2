// PluginEditor.h
// Editor UI for Awesome Studio plugins

#pragma once

#include <JuceHeader.h>

class PluginEditor : public juce::AudioProcessorEditor
{
public:
    PluginEditor(juce::AudioProcessor* processor);
    ~PluginEditor() override;

    // Editor resize
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    // UI components
    juce::Label titleLabel;
    juce::Slider gainSlider;
    juce::Label gainLabel;
};