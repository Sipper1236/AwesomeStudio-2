// Compressor.h
// Compressor effect processor for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>

class CompressorEffect : public juce::AudioProcessor
{
public:
    CompressorEffect();
    ~CompressorEffect() override;

    // Basic AudioProcessor interface
    const juce::String getName() const override { return "Compressor"; }
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
    // Compressor parameters
    float threshold;    // Threshold in dB (-60 to 0)
    float ratio;        // Compression ratio (1:1 to infinity:1)
    float attack;       // Attack time in ms (0.1 to 100)
    float release;      // Release time in ms (1 to 1000)
    float knee;         // Knee width in dB (0 to 30)
    float makeUpGain;   // Make-up gain in dB (-60 to +20)
    float mixDry;       // Dry mix level (0.0 to 1.0)
    float mixWet;       // Wet mix level (0.0 to 1.0)

    // Internal state
    float prevSampleL;
    float prevSampleR;
    float sampleRate;
    float gateThreshold; // Internal gate threshold

    // Peak detection
    float peakHoldL;
    float peakHoldR;
    float attackCoeff;
    float releaseCoeff;
};