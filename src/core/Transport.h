// Transport.h
// Transport module for Awesome Studio DAW (Record, Play, Stop)

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <map>
#include <string>

enum class TransportState
{
    Stopped,
    Playing,
    Recording,
    Paused
};

struct TransportInfo
{
    TransportState state;
    double position;      // Position in samples
    double playbackRate;  // Playback rate (1.0 = normal)
    double startTime;     // Start time in seconds
    double loopStart;     // Loop start in samples
    double loopEnd;       // Loop end in samples
    bool isLooping;       // Whether looping is enabled
    int64_t sampleRate;   // Sample rate
};

class Transport : public juce::AudioProcessor
{
public:
    Transport();
    ~Transport() override;

    // Basic AudioProcessor interface
    const juce::String getName() const override { return "Transport"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    // Parameter handling
    int getNumParameters() const override { return 0; }
    float getParameter(int index) const override { return 0.0f; }
    void setParameter(int index, float value) override {}
    const juce::String getParameterName(int index) const override { return {}; }

    // Audio block processing
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    // Prepare to play
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    // Release resources
    void releaseResources() override;

    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool editorIsChanging() const override { return false; }

    // Transport controls
    void setState(TransportState newState);
    TransportInfo getInfo() const;

    // Transport methods
    void startRecording();
    void stopRecording();
    void startPlayback();
    void stopPlayback();
    void setPosition(double samples);
    void setLoopPoints(double startSamples, double endSamples);
    void setLooping(bool enable);

private:
    TransportState state;
    double position;
    double playbackRate;
    double startTime;
    double loopStart;
    double loopEnd;
    bool isLooping;
    int64_t sampleRate;

    // Recording state
    bool isRecording;
    int64_t recordingStartPosition;
    int64_t recordingLength;

    // Time tracking
    double lastUpdateTime;
};