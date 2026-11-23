#pragma once

#include "Project.h"
#include "SynthRenderer.h"

#include <QString>
#include <array>
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
    void seekToBeat(double beat);
    bool isPlaying() const;
    double positionBeats() const;
    float trackPeakLevel(quint64 trackId) const;
    float busPeakLevel(size_t busIndex) const;
    float masterPeakLevel() const;
    void setLiveTrackMixOverride(quint64 trackId, int parameter, float value);
    void clearLiveTrackMixOverride(quint64 trackId, int parameter);
    void triggerLiveMidiNoteOn(int channel, int midiPitch, int velocity, const InstrumentTrack& track);
    void triggerLiveMidiNoteOff(int channel, int midiPitch);
    void triggerLiveMidiPitchBend(int channel, int value);
    void stopLiveMidiNotes();
    void triggerPreview(int midiPitch, int velocity = 96,
                        SynthWaveform waveform = SynthWaveform::Sine, float pan = 0.0f,
                        bool percussion = false);
    void triggerSamplePreview(std::shared_ptr<const AudioAsset> sample, int velocity = 96,
                              float pan = 0.0f, float gain = 1.0f);
    void setMasterGain(float gain);
    bool setAudioInputMonitoring(bool enabled);
    void pushAudioInputMonitorFrames(const float* interleavedSamples, qsizetype frameCount,
                                     int channelCount, int sampleRate);

private:
    static constexpr size_t maximumTrackCount = 256;
    static constexpr size_t maximumBusCount = 64;
    static constexpr size_t liveMidiQueueCapacity = 1024;
    static constexpr size_t maximumLiveMidiVoices = 128;
    static constexpr size_t audioMonitorBufferCapacity = 32768;
    class AudioStream;

    struct LiveMidiEvent
    {
        bool noteOn = false;
        int channel = 0;
        int pitch = 0;
        int velocity = 0;
        int pitchBendValue = 8192;
        size_t trackIndex = maximumTrackCount;
        bool percussion = false;
        bool pitchBend = false;
        SynthWaveform waveform = SynthWaveform::Sine;
        float gain = 1.0f;
        float pan = 0.0f;
        float attackSeconds = 0.008f;
        float decaySeconds = 0.0f;
        float sustainLevel = 1.0f;
        float releaseSeconds = 0.06f;
        std::shared_ptr<const AudioAsset> sample;
    };

    struct LiveMidiVoice
    {
        bool active = false;
        bool held = false;
        bool percussion = false;
        int channel = 0;
        int pitch = 0;
        size_t trackIndex = maximumTrackCount;
        qint64 startFrame = 0;
        qint64 releaseFrame = -1;
        qint64 attackFrames = 1;
        qint64 decayFrames = 0;
        qint64 releaseFrames = 1;
        quint64 serial = 0;
        double frequency = 440.0;
        double baseFrequency = 440.0;
        float amplitude = 0.0f;
        float sustainLevel = 1.0f;
        float releaseStartLevel = 1.0f;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
        SynthWaveform waveform = SynthWaveform::Sine;
        std::shared_ptr<const AudioAsset> sample;
    };

    struct AudioMonitorBufferEntry
    {
        StereoSample sample;
        quint64 generation = 0;
    };

    qint64 renderAudio(char* destination, qint64 maxBytes);
    void prepareNotes(const std::shared_ptr<const ProjectData>& project);
    bool enqueueLiveMidiEvent(const LiveMidiEvent& event);
    void applyLiveMidiEvent(const LiveMidiEvent& event, qint64 frame);
    void renderLiveMidiVoices(qint64 frame);
    void clearLiveMidiVoices();

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
    std::atomic<bool> seekPosition{false};
    std::atomic<qint64> requestedPositionFrames{0};
    std::atomic<qint64> publishedPositionFrames{0};
    std::atomic<qint64> audioClockFrames{0};
    std::atomic<float> masterGain{0.8f};
    std::array<std::atomic<float>, maximumTrackCount> liveTrackVolumes;
    std::array<std::atomic<float>, maximumTrackCount> liveTrackPans;
    std::array<LiveMidiEvent, liveMidiQueueCapacity> liveMidiEvents;
    std::array<LiveMidiVoice, maximumLiveMidiVoices> liveMidiVoices;
    std::array<float, 16> liveMidiChannelPitchBends{};
    std::array<int, 16> liveMidiChannelPitchBendValues{};
    std::array<StereoSample, maximumTrackCount> liveMidiTrackFrameSamples{};
    std::array<bool, maximumTrackCount> liveMidiTrackFrameTouched{};
    std::array<size_t, maximumLiveMidiVoices> liveMidiTouchedTracks{};
    std::array<float, maximumTrackCount> liveMidiBlockPeaks{};
    std::array<AudioMonitorBufferEntry, audioMonitorBufferCapacity> audioMonitorBuffer{};
    std::atomic<quint64> audioMonitorWriteIndex{0};
    std::atomic<quint64> audioMonitorReadIndex{0};
    std::atomic<quint64> audioMonitorGeneration{0};
    std::atomic<bool> audioInputMonitoring{false};
    double audioMonitorNextSourceFrame = 0.0;
    quint64 audioMonitorSourceFrameIndex = 0;
    StereoSample audioMonitorPreviousSample;
    bool audioMonitorHasPreviousSample = false;
    size_t liveMidiTouchedTrackCount = 0;
    std::atomic<size_t> liveMidiWriteIndex{0};
    std::atomic<size_t> liveMidiReadIndex{0};
    std::atomic<bool> liveMidiPanic{false};
    std::atomic<float> liveMidiPitchBendRangeSemitones{2.0f};
    float liveMidiAppliedPitchBendRangeSemitones = 2.0f;
    quint64 nextLiveMidiSerial = 1;
    std::array<std::atomic<float>, maximumTrackCount> trackPeakValues;
    std::array<std::atomic<float>, maximumBusCount> busPeakValues;
    std::atomic<float> masterPeakValue{0.0f};
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
