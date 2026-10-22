// Delay.cpp
// Delay effect processor implementation for Awesome Studio DAW

#include "Delay.h"
#include <JuceHeader.h>
#include <cmath>

//==============================================================================
DelayEffect::DelayEffect()
    : delayTime(0.5f), feedback(-12.0f), wetLevel(-6.0f), dryLevel(0.0f),
      lowCut(20.0f), highCut(20000.0f), syncSync(false), noteValue(4), // quarter note
      mixDry(0.5f), mixWet(0.5f),
      prevSampleL(0.0f), prevSampleR(0.0f)
{
    // Initialize delay lines (2 seconds at 44.1kHz stereo)
    maxDelaySamples = 44100 * 2; // 2 seconds
    delayLineL = juce::dsp::DelayLine<float, juce::dsp::LinearBlock<float>>(maxDelaySamples, {{}});
    delayLineR = juce::dsp::DelayLine<float, juce::dsp::LinearBlock<float>>(maxDelaySamples, {{}});

    // Initialize filters
    // Low-pass and high-pass for tone control
    // For simplicity, we'll set up the filters with the sample rate in prepareToPlay

    // Set default delay time to 500ms
    delayTime = 0.5f;
}

//==============================================================================
DelayEffect::~DelayEffect()
{
}

//==============================================================================
int DelayEffect::getNumParameters() const
{
    return 5; // delayTime, feedback, wetLevel, dryLevel, lowCut (highCut implicit)
}

//==============================================================================
float DelayEffect::getParameter(int index) const
{
    switch (index) {
        case 0: return delayTime;
        case 1: return juce::Decibels::gain(feedback);
        case 2: return juce::Decibels::gain(wetLevel);
        case 3: return juce::Decibels::gain(dryLevel);
        case 4: return lowCut;
        default: return 0.5f;
    }
}

//==============================================================================
void DelayEffect::setParameter(int index, float value)
{
    switch (index) {
        case 0: delayTime = jlimit(0.001f, 2.0f, value); break; // 0.1ms to 2000ms
        case 1: feedback = juce::Decibels::value(value); break; // -60dB to 0dB
        case 2: wetLevel = juce::Decibels::value(value); break; // -60dB to 0dB
        case 3: dryLevel = juce::Decibels::value(value); break; // -60dB to 0dB
        case 4: lowCut = jlimit(20.0f, 20000.0f, value); break; // Hz
    }
}

//==============================================================================
const juce::String DelayEffect::getParameterName(int index) const
{
    switch (index) {
        case 0: return "Delay Time";
        case 1: return "Feedback";
        case 2: return "Wet Level";
        case 3: return "Dry Level";
        case 4: return "Low Cut";
        default: return {};
    }
}

//==============================================================================
void DelayEffect::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = sampleRate;
    maxDelaySamples = (int)(sampleRate * 2.0); // 2 seconds max

    // Resize delay lines
    delayLineL.setSize(maxDelaySamples);
    delayLineR.setSize(maxDelaySamples);
    delayLineL.clear();
    delayLineR.clear();

    // Initialize low-pass and high-pass filters for tone control
    // Simple 1-pole filters
    juce::dsp::IIRFilter::Attributes lowPassAttrsL;
    lowPassAttrsL.coefficients = juce::dsp::IIRCoefficients::makeLowPass(sampleRate, highCut);
    highCutFilterL = juce::dsp::IIRFilter(lowPassAttrsL);

    juce::dsp::IIRFilter::Attributes lowPassAttrsR;
    lowPassAttrsR.coefficients = juce::dsp::IIRCoefficients::makeLowPass(sampleRate, highCut);
    highCutFilterR = juce::dsp::IIRFilter(lowPassAttrsR);

    // Low-cut (high-pass)
    juce::dsp::IIRFilter::Attributes highPassAttrsL;
    highPassAttrsL.coefficients = juce::dsp::IIRCoefficients::makeHighPass(sampleRate, lowCut);
    lowCutFilterL = juce::dsp::IIRFilter(highPassAttrsL);

    juce::dsp::IIRFilter::Attributes highPassAttrsR;
    highPassAttrsR.coefficients = juce::dsp::IIRCoefficients::makeHighPass(sampleRate, lowCut);
    lowCutFilterR = juce::dsp::IIRFilter(highPassAttrsR);

    // Initialize previous samples
    prevSampleL = 0.0f;
    prevSampleR = 0.0f;
}

//==============================================================================
void DelayEffect::releaseResources()
{
    // No special cleanup needed
}

//==============================================================================
void DelayEffect::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Get total number of channels
    int totalNumInputChannels = buffer.getNumChannels();
    int totalNumOutputChannels = jmax(totalNumInputChannels, 2);

    // Ensure output channels exist
    buffer.setTotalNumChannels(totalNumOutputChannels);

    // Calculate maximum delay in samples
    int maxDelaySamps = (int)(delayTime * sampleRate);
    maxDelaySamps = jlimit(1, maxDelaySamps, maxDelaySamples);

    // Apply low-cut filter to input channels first
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        auto* channelData = buffer.getWritePointer(ch);

        // Apply high-pass filter (low-cut)
        // lowCutFilterL.apply(*channelData, buffer.getNumSamples());

        // Store previous sample for feedback calculation
        if (buffer.getNumSamples() > 0) {
            prevSampleL = channelData[0];
            if (totalNumInputChannels > 1) {
                prevSampleR = channelData[1];
            }
        }
    }

    // Process delay for each channel
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        auto* channelData = buffer.getWritePointer(ch);

        // Apply high-pass filter
        // highCutFilterL.apply(*channelData, buffer.getNumSamples());

        // Apply delay effect
        for (int s = 0; s < buffer.getNumSamples(); ++s) {
            // Read from delay line (with interpolation for smoother sound)
            float delayReadSampleL = delayLineL.getSample(0); // Linear interpolation would be better
            float delayReadSampleR = delayLineR.getSample(0);

            // Calculate feedback amount
            float feedbackAmount = jlimit(-0.9f, 0.9f, feedback / 6.0f); // Convert dB to linear

            // Write to delay line (the current sample becomes the delayed sample for next iteration)
            // L channel
            delayLineL.setSample(0, s, prevSampleL + feedbackAmount * prevSampleL);
            // R channel
            delayLineR.setSample(0, s, prevSampleR + feedbackAmount * prevSampleR);

            // Store current sample as previous for next iteration
            prevSampleL = channelData[s];
            if (totalNumInputChannels > 1) {
                prevSampleR = (totalNumInputChannels > 1) ? channelData[s] : prevSampleL;
            }

            // Mix dry and wet signals
            float wetSample = channelData[s]; // The delayed sample
            float drySample = channelData[s]; // The original sample

            // Apply panning/dry-wet mix based on settings
            float dryMix = dryLevel / 0.0f; // Simplified - would use actual dB values
            float wetMix = wetLevel / 0.0f; // Simplified

            // Simple mix (would use actual mix values)
            channelData[s] = drySample + wetSample; // Full wet for now
        }
    }

    // Apply wet/dry mix at the end
    // for (int ch = 0; ch < totalNumOutputChannels; ++ch) {
    //     auto* channelData = buffer.getWritePointer(ch);
    //
    //     // Mix dry and wet
    //     for (int s = 0; s < buffer.getNumSamples(); ++s) {
    //         channelData[s] = channelData[s] * mixDry + processedSample * mixWet;
    //     }
    // }
}

//==============================================================================
juce::AudioProcessorEditor* DelayEffect::createEditor()
{
    // Return a custom delay editor UI
    // For now, return null - would need a custom editor component
    return nullptr;
}