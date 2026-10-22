// EQ3Band.cpp
// 3-band EQ effect processor implementation for Awesome Studio DAW

#include "EQ3Band.h"
#include <JuceHeader.h>
#include <cmath>

//==============================================================================
EQ3BandEffect::EQ3BandEffect()
    : lowGain(0.0f), midGain(0.0f), highGain(0.0f),
      lowFreq(200.0f), highFreq(4000.0f), qValue(1.0f),
      mixDry(0.5f), mixWet(0.5f), sampleRate(44100.0f)
{
    // Initialize IIR filters with appropriate coefficients
    // Low-cut (high-pass) at lowFreq
    // Mid band peak filter
    // High-cut (low-pass) at highFreq

    // Set up default filter parameters
    updateFilters();
}

EQ3BandEffect::~EQ3BandEffect()
{
}

//==============================================================================
float EQ3BandEffect::getParameter(int index) const
{
    switch (index) {
        case 0: return lowGain;
        case 1: return midGain;
        case 2: return highGain;
        case 3: return lowFreq;
        case 4: return highFreq;
        case 5: return qValue;
        default: return 0.5f;
    }
}

//==============================================================================
void EQ3BandEffect::setParameter(int index, float value)
{
    switch (index) {
        case 0: lowGain = jlimit(-24.0f, 24.0f, value); break;
        case 1: midGain = jlimit(-24.0f, 24.0f, value); break;
        case 2: highGain = jlimit(-24.0f, 24.0f, value); break;
        case 3: lowFreq = jlimit(20.0f, 500.0f, value); break;
        case 4: highFreq = jlimit(500.0f, 8000.0f, value); break;
        case 5: qValue = jlimit(0.5f, 20.0f, value); break;
    }

    updateFilters();
}

//==============================================================================
const juce::String EQ3BandEffect::getParameterName(int index) const
{
    switch (index) {
        case 0: return "Low Gain";
        case 1: return "Mid Gain";
        case 2: return "High Gain";
        case 3: return "Low Freq";
        case 4: return "High Freq";
        case 5: return "Q Value";
        default: return {};
    }
}

//==============================================================================
void EQ3BandEffect::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = (float)sampleRate;
    updateFilters();
}

//==============================================================================
void EQ3BandEffect::releaseResources()
{
    // No special cleanup needed
}

//==============================================================================
void EQ3BandEffect::updateFilters()
{
    // Design IIR filters for each EQ band

    // Low-cut (high-pass) filter at lowFreq
    juce::dsp::IIRFilter::Attributes highPassAttrs;
    highPassAttrs.coefficients = juce::dsp::IIRCoefficients::makeHighPass(sampleRate, lowFreq);
    lowCutFilter = juce::dsp::IIRFilter(highPassAttrs);

    // High-cut (low-pass) filter at highFreq
    juce::dsp::IIRFilter::Attributes lowPassAttrs;
    lowPassAttrs.coefficients = juce::dsp::IIRCoefficients::makeLowPass(sampleRate, highFreq);
    highCutFilter = juce::dsp::IIRFilter(lowPassAttrs);

    // Mid band peak filter between lowFreq and highFreq
    // Center frequency is geometric mean
    float centerFreq = std::sqrt(lowFreq * highFreq);
    float peakWidth = highFreq - lowFreq;

    juce::dsp::IIRFilter::Attributes peakAttrs;
    peakAttrs.coefficients = juce::dsp::IIRCoefficients::makePeakFilter(sampleRate, centerFreq, qValue);
    midFilter = juce::dsp::IIRFilter(peakAttrs);

    // Update the processor chain
    chain.reset();
    chain.append(lowCutFilter);
    chain.append(midFilter);
    chain.append(highCutFilter);
}

//==============================================================================
void EQ3BandEffect::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    int totalNumInputChannels = buffer.getNumChannels();
    int totalNumOutputChannels = jmax(totalNumInputChannels, 2);
    buffer.setTotalNumChannels(totalNumOutputChannels);

    // Process each channel through the 3-band EQ chain
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        auto* channelData = buffer.getWritePointer(ch);

        // Process samples through the filter chain
        for (int s = 0; s < buffer.getNumSamples(); ++s) {
            float sample = channelData[s];

            // Apply the filter chain
            float processed = chain.processSample(sample);

            // Apply gain to each band component
            // (Simplified - full implementation would split into bands, process gains, mix back)
            channelData[s] = processed * mixWet + sample * mixDry;
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* EQ3BandEffect::createEditor()
{
    // Return a custom EQ editor UI
    // For now, return null - would need a custom editor component
    return nullptr;
}