// Reverb.h
// Reverb effect processor for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>

class ReverbEffect : public juce::AudioProcessor
{
public:
    ReverbEffect();
    ~ReverbEffect() override;

    // Basic AudioProcessor interface
    const juce::String getName() const override { return "Reverb"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Parameter handling
    int getNumParameters() const override { return 4; }
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
    // Reverb parameters
    float roomSize;      // 0.0 to 1.0
    float dampening;     // 0.0 to 1.0
    float wetLevel;      // -60dB to 0dB (mixed level)
    float dryLevel;      // -60dB to 0dB (direct level)
    float width;         // 0.0 (mono) to 1.0 (stereo)
    float freeze;        // Freeze mode (0.0 to 1.0)
    float lowCut;        // Hz (highpass filter for feedback)
    float highCut;       // Hz (lowpass filter for feedback)

    // Internal state
    juce::dsp::DelayLine<float, juce::dsp::LinearBlock<float>> delayLine;
    juce::dsp::ProcessorChain<juce::d::IIRFilter, juce::d::IIRFilter, juce::d::IIRFilter> filters;

    // Reverb algorithms
    juce::dsp::ReverbAlgorithm algorithm;
    juce::dsp::Convolution convolution;

    // Filter coefficients
    juce::dsp::IIRCoefficients lowCutCoeffs;
    juce::dsp::IIRCoefficients highCutCoeffs;

    // Wet/dry mix
    float mixWet;
    float mixDry;
};