// PluginEditor.cpp
// Implementation of the plugin editor UI

#include "PluginEditor.h"

PluginEditor::PluginEditor(juce::AudioProcessor* processor)
    : juce::AudioProcessorEditor(processor)
{
    // Set up the editor size
    setSize(400, 200);

    // Add and configure UI components
    titleLabel.setText("Awesome Plugin", juce::dontSendNotification);
    titleLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel);

    gainSlider.setRange(0.0, 1.0, 0.001);
    gainSlider.setValue(0.5);
    gainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    addAndMakeVisible(gainSlider);

    gainLabel.setText("Gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(gainLabel);
}

PluginEditor::~PluginEditor()
{
}

void PluginEditor::resized()
{
    // Layout the UI components
    auto area = getLocalBounds().reduced(20);

    titleLabel.setBounds(area.removeFromTop(30).withX(20));

    auto sliderArea = area;
    gainSlider.setBounds(sliderArea.removeFromTop(50).withX(20));

    gainLabel.setBounds(gainSlider.getX(), gainSlider.getY() - 20, 50, 20);
}

void PluginEditor::paint(juce::Graphics& g)
{
    // Background
    g.fillAll(juce::Colours::black);

    // Border
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 1);

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(15.0f);
    g.drawText("Awesome Plugin", getLocalBounds(), juce::Justification::centredTop);
}