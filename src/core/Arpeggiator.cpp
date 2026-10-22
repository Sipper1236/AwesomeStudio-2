// Arpeggiator.cpp
// Arpeggiator and step sequencer implementation for Awesome Studio DAW

#include "Arpeggiator.h"
#include <JuceHeader.h>
#include <cmath>
#include <algorithm>

//==============================================================================
Arpeggiator::Arpeggiator()
    : rootNote(60), tempo(120.0f), timeSigNumerator(4), timeSigDenominator(4),
      swingAmount(0.0f), octaveRange(1), mode(0), currentStep(0),
      currentPatternIndex(0), samplesPerTick(0), sampleCounter(0)
{
    // Initialize patterns
    initializePatterns();

    // Set default pattern
    if (!availablePatterns.empty()) {
        currentPattern = availablePatterns[0];
    }
}

//==============================================================================
Arpeggiator::~Arpeggiator()
{
}

//==============================================================================
void Arpeggiator::initializePatterns()
{
    // Clear existing patterns
    availablePatterns.clear();

    // Add some built-in patterns
    availablePatterns.push_back({
        "Up",
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
        1,
        true
    });

    availablePatterns.push_back({
        "Down",
        {0, -1, -2, -3, -4, -5, -6, -7, -8, -9, -10, -11},
        1,
        true
    });

    availablePatterns.push_back({
        "Up/Down",
        {0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1},
        1,
        true
    });

    availablePatterns.push_back({
        "Random",
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
        1,
        true
    });

    availablePatterns.push_back({
        "Major",
        {0, 4, 7, 12},
        1,
        true
    });

    availablePatterns.push_back({
        "Minor",
        {0, 3, 7, 12},
        1,
        true
    });

    availablePatterns.push_back({
        "Seventh",
        {0, 4, 7, 10, 12},
        1,
        true
    });
}

//==============================================================================
void Arpeggiator::setRootNote(int noteNumber)
{
    rootNote = juce::jlimit(0, 127, noteNumber);
}

//==============================================================================
void Arpeggiator::setPattern(const ArpeggiatorPattern& pattern)
{
    currentPattern = pattern;
}

//==============================================================================
void Arpeggiator::setTempo(float bpm)
{
    tempo = juce::jlimit(20.0f, 300.0f, bpm);

    // Calculate samples per tick
    double samplesPerMinute = 60.0 * 44100.0;
    double ticksPerMinute = tempo * timeSigNumerator;
    samplesPerTick = (int)(samplesPerMinute / ticksPerMinute);
}

//==============================================================================
void Arpeggiator::setTimeSignature(int numerator, int denominator)
{
    timeSigNumerator = juce::jlimit(1, 16, numerator);
    timeSigDenominator = juce::jlimit(1, 16, denominator);

    // Recalculate samples per tick
    setTempo(tempo);
}

//==============================================================================
void Arpeggiator::setSwing(float amount)
{
    swingAmount = juce::jlimit(0.0f, 1.0f, amount);
}

//==============================================================================
void Arpeggiator::setOctaveRange(int range)
{
    octaveRange = juce::jlimit(1, 4, range);
}

//==============================================================================
void Arpeggiator::setMode(int newMode)
{
    mode = juce::jlimit(0, 3, newMode);
}

//==============================================================================
void Arpeggiator::setSteps(const std::vector<Step>& steps)
{
    currentSteps = steps;
}

//==============================================================================
void Arpeggiator::processMidiEvents(juce::MidiBuffer& midiMessages, int startSample, int numSamples)
{
    // Process arpeggiator
    processArpeggiator(midiMessages, startSample, numSamples);

    // Process step sequencer
    processStepSequencer(midiMessages, startSample, numSamples);
}

//==============================================================================
void Arpeggiator::processArpeggiator(juce::MidiBuffer& midiMessages, int startSample, int numSamples)
{
    if (currentPattern.pattern.empty()) return;

    // Calculate number of ticks in this buffer
    int ticksInBuffer = numSamples / samplesPerTick;

    // Process each tick
    for (int i = 0; i < ticksInBuffer; ++i) {
        int samplePos = startSample + i * samplesPerTick;

        // Apply swing if needed
        if (swingAmount > 0.0f && currentPatternIndex % 2 == 1) {
            samplePos += (int)(swingAmount * samplesPerTick);
        }

        // Get note offset from pattern
        int noteOffset = currentPattern.pattern[currentPatternIndex];

        // Apply octave range
        int octave = juce::Random::getSystemRandom().nextInt(octaveRange);
        noteOffset += octave * 12;

        // Calculate final note number
        int noteNumber = rootNote + noteOffset;
        noteNumber = juce::jlimit(0, 127, noteNumber);

        // Add note on message
        midiMessages.addEvent(juce::MidiMessage::noteOn(1, noteNumber, 0.8f), samplePos);

        // Add note off message
        midiMessages.addEvent(juce::MidiMessage::noteOff(1, noteNumber), samplePos + samplesPerTick / 2);

        // Advance pattern index
        currentPatternIndex++;
        if (currentPatternIndex >= (int)currentPattern.pattern.size()) {
            if (currentPattern.loop) {
                currentPatternIndex = 0;
            } else {
                currentPatternIndex = (int)currentPattern.pattern.size() - 1;
            }
        }
    }
}

//==============================================================================
void Arpeggiator::processStepSequencer(juce::MidiBuffer& midiMessages, int startSample, int numSamples)
{
    if (currentSteps.empty()) return;

    // Calculate number of ticks in this buffer
    int ticksInBuffer = numSamples / samplesPerTick;

    // Process each tick
    for (int i = 0; i < ticksInBuffer; ++i) {
        int samplePos = startSample + i * samplesPerTick;

        // Get current step
        const Step& step = currentSteps[currentStep];

        // If not a rest, add note
        if (step.noteNumber >= 0) {
            // Calculate final note number
            int noteNumber = step.noteNumber;
            noteNumber = juce::jlimit(0, 127, noteNumber);

            // Calculate velocity
            float velocity = step.velocity / 127.0f;
            if (step.accent) {
                velocity *= 1.2f; // Accented notes are louder
            }

            // Add note on message
            midiMessages.addEvent(juce::MidiMessage::noteOn(1, noteNumber, velocity), samplePos);

            // Add note off message
            int durationSamples = step.duration * samplesPerTick;
            midiMessages.addEvent(juce::MidiMessage::noteOff(1, noteNumber), samplePos + durationSamples);
        }

        // Advance step index
        currentStep++;
        if (currentStep >= (int)currentSteps.size()) {
            currentStep = 0;
        }
    }
}
