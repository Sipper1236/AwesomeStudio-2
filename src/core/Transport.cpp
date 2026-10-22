// Transport.cpp
// Implementation of the transport module

#include "Transport.h"
#include <juce_audio_processor.h>

//==============================================================================
Transport::Transport()
    : state(TransportState::Stopped), position(0.0), playbackRate(1.0),
      startTime(0.0), loopStart(0.0), loopEnd(0.0), isLooping(false),
      sampleRate(44100), isRecording(false), recordingStartPosition(0),
      recordingLength(0), lastUpdateTime(0.0)
{
}

//==============================================================================
Transport::~Transport()
{
}

//==============================================================================
void Transport::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Update position based on playback state
    double currentTime = Time::getCurrentTime() - startTime;

    switch (state) {
        case TransportState::Playing:
            position += playbackRate * (currentTime - lastUpdateTime);
            lastUpdateTime = currentTime;
            break;

        case TransportState::Recording:
            position += playbackRate * (currentTime - lastUpdateTime);
            lastUpdateTime = currentTime;
            // In a real implementation, we'd write to a buffer here
            break;

        case TransportState::Paused:
            // Position stays fixed
            break;

        case TransportState::Stopped:
            position = 0.0;
            lastUpdateTime = currentTime;
            break;
    }

    // Ensure position stays within reasonable bounds
    position = jlimit(0.0, 1e10, position);

    // Clear the buffer to silence (or could fill with zeros)
    buffer.clear();
}

//==============================================================================
void Transport::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = (int64)sampleRate;
    lastUpdateTime = Time::getCurrentTime();

    // Reset position at start of play
    if (state == TransportState::Playing || state == TransportState::Recording) {
        position = 0.0;
    }
}

//==============================================================================
void Transport::releaseResources()
{
    // No special cleanup needed
}

//==============================================================================
juce::AudioProcessorEditor* Transport::createEditor()
{
    // Return a custom transport UI editor
    // For now, return null
    return nullptr;
}

//==============================================================================
void Transport::setState(TransportState newState)
{
    state = newState;

    // Reset position when starting/stopping
    if (newState == TransportState::Stopped) {
        position = 0.0;
        lastUpdateTime = Time::getCurrentTime();
    }
    else if (newState == TransportState::Playing || newState == TransportState::Recording) {
        if (state == TransportState::Stopped) {
            position = 0.0;
            lastUpdateTime = Time::getCurrentTime();
        }
    }
}

//==============================================================================
TransportInfo Transport::getInfo() const
{
    TransportInfo info;
    info.state = state;
    info.position = position;
    info.playbackRate = playbackRate;
    info.startTime = startTime;
    info.loopStart = loopStart;
    info.loopEnd = loopEnd;
    info.isLooping = isLooping;
    info.sampleRate = sampleRate;
    return info;
}

//==============================================================================
void Transport::startRecording()
{
    isRecording = true;
    recordingStartPosition = position;
    recordingLength = 0;
    std::cout << "Recording started at position " << position << std::endl;
}

//==============================================================================
void Transport::stopRecording()
{
    isRecording = false;
    recordingLength = position - recordingStartPosition;
    std::cout << "Recording stopped. Record length: " << recordingLength << " samples" << std::endl;
}

//==============================================================================
void Transport::startPlayback()
{
    setState(TransportState::Playing);
    std::cout << "Playback started" << std::endl;
}

//==============================================================================
void Transport::stopPlayback()
{
    setState(TransportState::Stopped);
    std::cout << "Playback stopped" << std::endl;
}

//==============================================================================
void Transport::setPosition(double samples)
{
    position = jlimit(0.0, 1e10, samples);
    lastUpdateTime = Time::getCurrentTime();
}

//==============================================================================
void Transport::setLoopPoints(double startSamples, double endSamples)
{
    loopStart = startSamples;
    loopEnd = endSamples;
    isLooping = (loopEnd > loopStart);
}

//==============================================================================
void Transport::setLooping(bool enable)
{
    isLooping = enable;
}