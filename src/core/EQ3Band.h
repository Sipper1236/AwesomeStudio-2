// EQ3Band.h
// 3-band EQ effect processor for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>

class EQ3BandEffect : public juce::AudioProcessor
{
public:
    EQ3BandEffect();
    ~EQ3BandEffect() override;

    // Basic AudioProcessor interface
    const juce::String getName() const override { return "EQ3Band"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Parameter handling
    int getNumParameters() const override { return 6; }
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
    // EQ bands: low (high-pass crossover), mid, high (low-pass crossover)
    float lowGain;      // Low band gain in dB (-24 to +24)
    float midGain;      // Mid band gain in dB (-24 to +24)
    float highGain;     // High band gain in dB (-24 to +24)
    float lowFreq;      // Low-mid crossover frequency (20 to 500 Hz)
    float highFreq;     // Mid-high crossover frequency (500 to 8000 Hz)
    float qValue;       // Q factor for peak filters (0.5 to 20.0)
    float mixDry;       // Dry mix level (0.0 to 1.0)
    float mixWet;       // Wet mix level (0.0 to 1.0)

    // JUCE filters for each band
    juce::dsp::IIRFilter lowCutFilter;
    juce::dsp::IIRFilter midFilter;
    juce::dsp::IIRFilter highCutFilter;
    juce::dsp::ProcessorChain<juce::dsp::IIRFilter, juce::dsp::IIRFilter, juce::dsp::IIRFilter, juce::dsp::IIRFilter> chain;

    float sampleRate;
};