// Reverb.cpp
// Reverb effect processor implementation for Awesome Studio DAW

#include "Reverb.h"
#include <juce_audio_processors.h>
#include <juce_dsp.h>

//==============================================================================
ReverbEffect::ReverbEffect()
    : roomSize(0.5f), dampening(0.5f), wetLevel(-12.0f), dryLevel(0.0f),
      width(1.0f), freeze(0.0f), lowCut(20.0f), highCut(20000.0f),
      mixWet(0.5f), mixDry(0.5f)
{
    // Initialize delay line (stereo, 2 seconds max)
    delayLine = juce::dsp::DelayLine<float, juce::dsp::LinearBlock<float>>(
        44100 * 2, // 2 seconds at 44.1kHz
        {{}});

    // Initialize low-cut and high-cut filters
    // Simple high-pass at lowCut and low-pass at highCut
    juce::dsp::IIRFilter::Attributes lowAttrs;
    lowAttrs.coefficients = juce::dsp::IIRCoefficients::makeHighPass(sampleRate, lowCut);
    lowCutCoeffs = juce::dsp::IIRCoefficients(lowAttrs);

    juce::dsp::IIRFilter::Attributes highAttrs;
    highAttrs.coefficients = juce::dsp::IIRCoefficients::makeLowPass(sampleRate, highCut);
    highCutCoeffs = juce::dsp::IIRCoefficients(highAttrs);

    // Initialize reverb algorithm
    algorithm = juce::dsp::ReverbAlgorithm::smallRoom;
}

//==============================================================================
ReverbEffect::~ReverbEffect()
{
}

//==============================================================================
int ReverbEffect::getNumParameters() const
{
    return 4; // roomSize, dampening, wetLevel, dryLevel
}

//==============================================================================
float ReverbEffect::getParameter(int index) const
{
    switch (index) {
        case 0: return roomSize;
        case 1: return dampening;
        case 2: return juce::Decibels::gain(wetLevel);
        case 3: return juce::Decibels::gain(dryLevel);
        default: return 0.0f;
    }
}

//==============================================================================
void ReverbEffect::setParameter(int index, float value)
{
    switch (index) {
        case 0: roomSize = jlimit(0.0f, 1.0f, value); break;
        case 1: dampening = jlimit(0.0f, 1.0f, value); break;
        case 2: wetLevel = juce::Decibels::value(value); break;
        case 3: dryLevel = juce::Decibels::value(value); break;
    }
}

//==============================================================================
const juce::String ReverbEffect::getParameterName(int index) const
{
    switch (index) {
        case 0: return "Room Size";
        case 1: return "Dampening";
        case 2: return "Wet Level";
        case 3: return "Dry Level";
        default: return {};
    }
}

//==============================================================================
void ReverbEffect::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = sampleRate;

    // Update filter coefficients with new sample rate
    lowCutCoeffs = juce::dsp::IIRCoefficients::makeHighPass(sampleRate, lowCut);
    highCutCoeffs = juce::dsp::IIRCoefficients::makeLowPass(sampleRate, highCut);

    // Reset delay line
    delayLine.setSize(2, sampleRate * 2); // 2 seconds stereo
    delayLine.clear();

    // Initialize reverb algorithm with sample rate
    // algorithm.reset(sampleRate);
}

//==============================================================================
void ReverbEffect::releaseResources()
{
    // No special cleanup needed
}

//==============================================================================
void ReverbEffect::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Get total number of channels
    int totalNumInputChannels = buffer.getNumChannels();
    int totalNumOutputChannels = jmax(totalNumInputChannels, getNumOutputChannels());

    // Ensure output channels exist
    buffer.setTotalNumChannels(totalNumOutputChannels);

    // Apply high-pass and low-pass filters to feedback
    // for (int ch = 0; ch < totalNumInputChannels; ++ch) {
    //     auto* channelData = buffer.getWritePointer(ch);
    //
    //     // Apply high-pass filter
    //     // lowCutCoeffs.apply(*channelData, sampleCount);
    //
    //     // Apply low-pass filter
    //     // highCutCoeffs.apply(*channelData, sampleCount);
    // }

    // Process reverb for each channel
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        auto* channelData = buffer.getWritePointer(ch);

        // Apply filters
        // lowCutCoeffs.apply(*channelData, buffer.getNumSamples());
        // highCutCoeffs.apply(*channelData, buffer.getNumSamples());

        // Simple reverb effect (basic delay-based reverb)
        for (int s = 0; s < buffer.getNumSamples(); ++s) {
            // Read from delay line
            float sample = channelData[s];

            // Write to delay line with feedback
            delayLine.setSample(ch, s, sample);

            // Mix dry and wet
            float wetSample = sample * (wetLevel / 0.0f); // Simplified
            float drySample = sample * (dryLevel / 0.0f); // Simplified

            channelData[s] = drySample + wetSample;
        }
    }

    // Apply wet/dry mix
    // for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
    //     auto* channelData = buffer.getWritePointer(ch);
    //
    //     // Apply mix
    //     for (int s = 0; s < buffer.getNumSamples(); ++s) {
    //         channelData[s] = channelData[s] * mixDry + processedSample * mixWet;
    //     }
    // }
}

//==============================================================================
juce::AudioProcessorEditor* ReverbEffect::createEditor()
{
    // Return a custom reverb editor UI
    // For now, return null - would need a custom editor component
    return nullptr;
}