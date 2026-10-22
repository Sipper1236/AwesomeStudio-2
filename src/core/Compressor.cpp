// Compressor.cpp
// Compressor effect processor implementation for Awesome Studio DAW

#include "Compressor.h"
#include <JuceHeader.h>
#include <cmath>

//==============================================================================
CompressorEffect::CompressorEffect()
    : threshold(-12.0f), ratio(4.0f), attack(10.0f), release(100.0f),
      knee(5.0f), makeUpGain(0.0f), mixDry(0.5f), mixWet(0.5f),
      prevSampleL(0.0f), prevSampleR(0.0f), sampleRate(44100.0f),
      peakHoldL(0.0f), peakHoldR(0.0f)
{
    // Calculate attack and release coefficients
    attackCoeff = std::exp(-1.0f / (attack * 0.001f * sampleRate));
    releaseCoeff = std::exp(-1.0f / (release * 0.001f * sampleRate));
}

CompressorEffect::~CompressorEffect()
{
}

//==============================================================================
float CompressorEffect::getParameter(int index) const
{
    switch (index) {
        case 0: return threshold;
        case 1: return ratio;
        case 2: return attack;
        case 3: return release;
        default: return 0.5f;
    }
}

//==============================================================================
void CompressorEffect::setParameter(int index, float value)
{
    switch (index) {
        case 0: threshold = jlimit(-60.0f, 0.0f, value); break;
        case 1: ratio = jlimit(1.0f, 20.0f, value); break;
        case 2: attack = jlimit(0.1f, 100.0f, value); break;
        case 3: release = jlimit(1.0f, 1000.0f, value); break;
    }

    // Recalculate coefficients
    attackCoeff = std::exp(-1.0f / (attack * 0.001f * sampleRate));
    releaseCoeff = std::exp(-1.0f / (release * 0.001f * sampleRate));
}

//==============================================================================
const juce::String CompressorEffect::getParameterName(int index) const
{
    switch (index) {
        case 0: return "Threshold";
        case 1: return "Ratio";
        case 2: return "Attack";
        case 3: return "Release";
        default: return {};
    }
}

//==============================================================================
void CompressorEffect::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = (float)sampleRate;
    peakHoldL = 0.0f;
    peakHoldR = 0.0f;
}

//==============================================================================
void CompressorEffect::releaseResources()
{
    // No special cleanup needed
}

//==============================================================================
void CompressorEffect::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    int totalNumInputChannels = buffer.getNumChannels();
    int totalNumOutputChannels = jmax(totalNumInputChannels, 2);
    buffer.setTotalNumChannels(totalNumOutputChannels);

    // Calculate maximum gain reduction
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        auto* channelData = buffer.getWritePointer(ch);

        for (int s = 0; s < buffer.getNumSamples(); ++s) {
            // Detect peak sample
            float absSample = std::abs(channelData[s]);

            // Peak hold with time constant
            if (absSample > peakHoldL + 0.001f) { // simplified per-channel
                peakHoldL = absSample;
            }

            // Calculate gain reduction based on threshold
            float inputAbs = jabs(channelData[s]);
            float overThreshold = inputAbs - jabs(threshold); // simplified

            if (overThreshold > 0) {
                // Apply compression: output = threshold + (input - threshold) / ratio
                // With knee smoothing around threshold
                float inputDbs = jdbfromgain(inputAbs);
                float thresholdDbs = jdbfromgain(jabs(threshold));
                float diff = inputDbs - thresholdDbs;

                if (diff > knee * 0.5f) {
                    // Hard knee above threshold
                    float reduction = diff / ratio;
                    channelData[s] = jdbtogain(thresholdDbs + reduction) * (channelData[s] / inputAbs);
                }
            }

            // Apply make-up gain
            channelData[s] *= (1.0f + makeUpGain / 20.0f);
        }
    }

    // Apply dry/wet mix
    if (mixWet != 1.0f) {
        for (int ch = 0; ch < totalNumInputChannels; ++ch) {
            auto* channelData = buffer.getWritePointer(ch);
            for (int s = 0; s < buffer.getNumSamples(); ++s) {
                channelData[s] = channelData[s] * mixDry + channelData[s] * mixWet;
            }
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* CompressorEffect::createEditor()
{
    // Return a custom compressor editor UI
    // For now, return null - would need a custom editor component
    return nullptr;
}