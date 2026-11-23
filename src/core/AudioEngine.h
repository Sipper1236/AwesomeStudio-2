#pragma once

#include "Project.h"
#include "SynthRenderer.h"

#include <QString>
#include <atomic>
#include <memory>

class QAudioFormat;
class QAudioSink;

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    bool initialize();
    bool isAudioAvailable() const;
    QString deviceName() const;
    QString errorMessage() const;

    void setProject(const ProjectData& project);
    void startPlayback();
    void stopPlayback();
    bool isPlaying() const;
    double positionBeats() const;
    void triggerPreview(int midiPitch, int velocity = 96,
                        SynthWaveform waveform = SynthWaveform::Sine, float pan = 0.0f,
                        bool percussion = false);
    void triggerSamplePreview(std::shared_ptr<const AudioAsset> sample, int velocity = 96,
                              float pan = 0.0f, float gain = 1.0f);
    void setMasterGain(float gain);

private:
    class AudioStream;

    qint64 renderAudio(char* destination, qint64 maxBytes);
    void prepareNotes(const std::shared_ptr<const ProjectData>& project);

    std::shared_ptr<const ProjectData> projectSnapshot;
    std::shared_ptr<const ProjectData> preparedProject;
    SynthRenderer renderer;
    std::unique_ptr<AudioStream> stream;
    std::unique_ptr<QAudioSink> sink;
    std::unique_ptr<QAudioFormat> outputFormat;

    QString outputDeviceName;
    QString initializationError;
    double outputSampleRate = 48000.0;
    int outputChannels = 2;
    int bytesPerFrame = 0;
    std::atomic<bool> playing{false};
    std::atomic<bool> resetPosition{true};
    std::atomic<qint64> publishedPositionFrames{0};
    std::atomic<qint64> audioClockFrames{0};
    std::atomic<float> masterGain{0.8f};
    std::atomic<int> previewPitch{-1};
    std::atomic<int> previewVelocity{96};
    std::atomic<double> previewFrequency{440.0};
    std::atomic<int> previewWaveform{static_cast<int>(SynthWaveform::Sine)};
    std::atomic<bool> previewPercussion{false};
    std::atomic<float> previewLeftGain{0.70710678f};
    std::atomic<float> previewRightGain{0.70710678f};
    std::atomic<qint64> previewStartFrame{0};
    std::atomic<qint64> previewEndFrame{0};
    std::shared_ptr<const AudioAsset> previewSampleAsset;
    std::atomic<float> previewSampleGain{1.0f};

    qint64 playbackFrame = 0;
};
