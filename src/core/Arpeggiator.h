// Arpeggiator.h
// Arpeggiator and step sequencer for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>

// Arpeggiator pattern structure
struct ArpeggiatorPattern
{
    juce::String name;
    std::vector<int> pattern; // Note offsets (0 = root, 1 = +1 semitone, etc.)
    int octaveRange; // Number of octaves to span
    bool loop; // Whether to loop the pattern
};

// Step sequencer step structure
struct Step
{
    int noteNumber; // MIDI note number (-1 for rest)
    int velocity; // Note velocity (0-127)
    int duration; // Duration in ticks
    bool accent; // Whether to accent this step
    juce::String chordSymbol; // Chord symbol for this step
};

// Arpeggiator and step sequencer class
class Arpeggiator
{
public:
    Arpeggiator();
    ~Arpeggiator();

    // Set root note
    void setRootNote(int noteNumber);

    // Set pattern
    void setPattern(const ArpeggiatorPattern& pattern);

    // Set tempo (BPM)
    void setTempo(float bpm);

    // Set time signature
    void setTimeSignature(int numerator, int denominator);

    // Set swing amount (0.0 to 1.0)
    void setSwing(float amount);

    // Set octave range
    void setOctaveRange(int range);

    // Set arpeggiator mode
    void setMode(int mode); // 0=up, 1=down, 2=up/down, 3=random

    // Set step sequencer steps
    void setSteps(const std::vector<Step>& steps);

    // Process MIDI events
    void processMidiEvents(juce::MidiBuffer& midiMessages, int startSample, int numSamples);

    // Get current pattern
    const ArpeggiatorPattern& getCurrentPattern() const { return currentPattern; }

    // Get available patterns
    const std::vector<ArpeggiatorPattern>& getAvailablePatterns() const { return availablePatterns; }

    // Get current steps
    const std::vector<Step>& getCurrentSteps() const { return currentSteps; }

private:
    // Internal state
    int rootNote;
    ArpeggiatorPattern currentPattern;
    std::vector<ArpeggiatorPattern> availablePatterns;
    std::vector<Step> currentSteps;
    float tempo;
    int timeSigNumerator;
    int timeSigDenominator;
    float swingAmount;
    int octaveRange;
    int mode;
    int currentStep;
    int currentPatternIndex;
    int samplesPerTick;
    int sampleCounter;

    // Internal methods
    void initializePatterns();
    void processArpeggiator(juce::MidiBuffer& midiMessages, int startSample, int numSamples);
    void processStepSequencer(juce::MidiBuffer& midiMessages, int startSample, int numSamples);
};
