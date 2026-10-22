// Delay.h
// Delay effect processor for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>

class DelayEffect : public juce::AudioProcessor
{
public:
    DelayEffect();
    ~DelayEffect() override;

    // Basic AudioProcessor interface
    const juce::String getName() const override { return "Delay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Parameter handling
    int getNumParameters() const override { return 5; }
    float getParameter(int index) const override;
    void setParameter(int index, float value) override;
    const juce::String getParameterName(int index) const override;

    // Audio block processing
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    // Prepare to play
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    // Release resources
    void releaseResources() override;

    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool editorIsChanging() const override { return false; }

private:
    // Delay parameters
    float delayTime;     // 0.1ms to 2000ms
    float feedback;      // -60dB to 0dB (regeneration)
    float wetLevel;      // -60dB to 0dB (mixed level)
    float dryLevel;      // -60dB to 0dB (direct level)
    float lowCut;        // Hz (highpass filter for repeats)
    float highCut;       // Hz (lowpass filter for repeats)
    bool syncSync;       // Note sync enabled
    float noteValue;     // Note value when synced (whole, half, quarter, etc.)
    float mixDry;        // Dry mix level (0.0 to 1.0)
    float mixWet;        // Wet mix level (0.0 to 1.0)

    // Internal state
    juce::dsp::DelayLine<float, juce::dsp::LinearBlock<float>> delayLineL;
    juce::dsp::DelayLine<float, juce::dsp::LinearBlock<float>> delayLineR;
    juce::dsp::IIRFilter lowCutFilterL;
    juce::dsp::IIRFilter lowCutFilterR;
    juce::dsp::IIRFilter highCutFilterL;
    juce::dsp::IIRFilter highCutFilterR;
    float prevSampleL;
    float prevSampleR;
    int maxDelaySamples;

    // Sync settings
    double sampleRate;
    int syncNoteDenominator; // 1=whole, 2=half, 4=quarter, 8=eighth, 16=sixteenth
};