#pragma once

#include "Project.h"
#include "SynthRenderer.h"

#include <QString>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

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
    void setLiveSynthFilterOverride(quint64 trackId, int parameter, float value);
    void clearLiveSynthFilterOverride(quint64 trackId, int parameter);
    void setLivePluginParameterOverride(quint64 trackId, qsizetype effectIndex,
                                        qsizetype parameterIndex, float value);
    void clearLivePluginParameterOverride(quint64 trackId, qsizetype effectIndex,
                                          qsizetype parameterIndex);
    void capturePluginStates(ProjectData& project);
    void servicePluginCallbacks();
    void triggerLiveMidiNoteOn(int channel, int midiPitch, int velocity, const InstrumentTrack& track);
    void triggerLiveMidiNoteOff(int channel, int midiPitch);
    void triggerLiveMidiPitchBend(int channel, int value,
                                  const InstrumentTrack* track = nullptr);
    void triggerPluginInstrumentPreview(int midiPitch, int velocity, const InstrumentTrack& track);
    void stopLiveMidiNotes();
    void triggerPreview(int midiPitch, int velocity = 96,
                        SynthWaveform waveform = SynthWaveform::Sine, float pan = 0.0f,
                        bool percussion = false, bool secondOscillatorEnabled = false,
                        SynthWaveform secondOscillatorWaveform = SynthWaveform::Saw,
                        float secondOscillatorDetuneCents = 0.0f,
                        float secondOscillatorMix = 0.5f,
                        bool synthFilterEnabled = false,
                        float synthFilterCutoffHz = 20000.0f,
                        float synthFilterResonance = 0.707f,
                        SynthFilterType synthFilterType = SynthFilterType::LowPass,
                        bool synthFilterEnvelopeEnabled = false,
                        float synthFilterEnvelopeAmountSemitones = 24.0f,
                        float synthFilterEnvelopeAttackSeconds = 0.008f,
                        float synthFilterEnvelopeDecaySeconds = 0.2f,
                        float synthFilterEnvelopeSustainLevel = 0.0f,
                        float synthFilterEnvelopeReleaseSeconds = 0.2f,
                        float attackSeconds = 0.008f,
                        float decaySeconds = 0.0f,
                        float sustainLevel = 1.0f,
                        float releaseSeconds = 0.06f);
    void triggerSamplePreview(std::shared_ptr<const AudioAsset> sample, int velocity = 96,
                              float pan = 0.0f, float gain = 1.0f,
                              float startPositionPercent = 0.0f,
                              float endPositionPercent = 100.0f,
                              float tuningSemitones = 0.0f,
                              bool loopEnabled = false,
                              float attackSeconds = 0.008f,
                              float decaySeconds = 0.0f,
                              float sustainLevel = 1.0f,
                              float releaseSeconds = 0.06f);
    void setMasterGain(float gain);
    bool setAudioInputMonitoring(bool enabled);
    void setAudioInputMonitorTrack(quint64 trackId);
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
        std::int32_t noteId = 0;
        int pitchBendValue = 8192;
        size_t trackIndex = maximumTrackCount;
        bool percussion = false;
        bool pitchBend = false;
        bool pluginInstrument = false;
        bool preview = false;
        bool filterEnvelopeEnabled = false;
        SynthWaveform waveform = SynthWaveform::Sine;
        SynthWaveform secondOscillatorWaveform = SynthWaveform::Saw;
        float secondOscillatorDetuneCents = 0.0f;
        float secondOscillatorMix = 0.5f;
        bool secondOscillatorEnabled = false;
        float gain = 1.0f;
        float pan = 0.0f;
        float attackSeconds = 0.008f;
        float decaySeconds = 0.0f;
        float sustainLevel = 1.0f;
        float releaseSeconds = 0.06f;
        float filterEnvelopeAmountSemitones = 24.0f;
        float filterEnvelopeAttackSeconds = 0.008f;
        float filterEnvelopeDecaySeconds = 0.2f;
        float filterEnvelopeSustainLevel = 0.0f;
        float filterEnvelopeReleaseSeconds = 0.2f;
        std::shared_ptr<const AudioAsset> sample;
        qint64 sampleStartFrame = 0;
        qint64 sampleEndFrame = 0;
        double samplePlaybackRate = 1.0;
        bool sampleLoopEnabled = false;
    };

    struct LiveMidiVoice
    {
        bool active = false;
        bool held = false;
        bool percussion = false;
        bool pluginInstrument = false;
        bool preview = false;
        bool filterEnvelopeEnabled = false;
        int channel = 0;
        int pitch = 0;
        std::int32_t pluginNoteId = 0;
        size_t trackIndex = maximumTrackCount;
        qint64 startFrame = 0;
        qint64 releaseFrame = -1;
        qint64 attackFrames = 1;
        qint64 decayFrames = 0;
        qint64 releaseFrames = 1;
        qint64 filterEnvelopeAttackFrames = 1;
        qint64 filterEnvelopeDecayFrames = 0;
        qint64 filterEnvelopeReleaseFrames = 1;
        quint64 serial = 0;
        double frequency = 440.0;
        double baseFrequency = 440.0;
        float amplitude = 0.0f;
        float sustainLevel = 1.0f;
        float releaseStartLevel = 1.0f;
        float filterEnvelopeAmountSemitones = 24.0f;
        float filterEnvelopeSustainLevel = 0.0f;
        float filterEnvelopeReleaseStartLevel = 1.0f;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
        SynthWaveform waveform = SynthWaveform::Sine;
        SynthWaveform secondOscillatorWaveform = SynthWaveform::Saw;
        float secondOscillatorDetuneCents = 0.0f;
        float secondOscillatorMix = 0.5f;
        bool secondOscillatorEnabled = false;
        std::shared_ptr<const AudioAsset> sample;
        qint64 sampleStartFrame = 0;
        qint64 sampleEndFrame = 0;
        qint64 sampleDurationFrames = 0;
        double samplePlaybackRate = 1.0;
        bool sampleLoopEnabled = false;
    };

    struct AudioMonitorBufferEntry
    {
        StereoSample sample;
        quint64 generation = 0;
    };

    qint64 renderAudio(char* destination, qint64 maxBytes);
    std::atomic<bool> pluginStateCaptureInProgress{false};
    std::atomic<size_t> activeRenderCallbacks{0};
    bool enqueueLiveMidiEvent(const LiveMidiEvent& event);
    void applyLiveMidiEvent(const LiveMidiEvent& event, qint64 frame, SynthRenderer& renderer);
    void renderLiveMidiVoices(qint64 frame, SynthRenderer& renderer);
    void clearLiveMidiVoices();

    struct PreparedRenderState
    {
        std::shared_ptr<const ProjectData> project;
        std::shared_ptr<SynthRenderer> renderer;
    };
    std::shared_ptr<const PreparedRenderState> preparedRenderState;
    std::vector<std::shared_ptr<const PreparedRenderState>> retiredRenderStates;
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
    std::array<std::atomic<float>, maximumTrackCount> liveSynthFilterCutoffOverrides;
    std::array<std::atomic<float>, maximumTrackCount> liveSynthFilterResonanceOverrides;
    std::atomic<quint64> livePluginTrackId{0};
    std::atomic<qsizetype> livePluginEffectIndex{-1};
    std::atomic<qsizetype> livePluginParameterIndex{-1};
    std::atomic<float> livePluginParameterValue{0.0f};
    std::atomic<bool> livePluginParameterOverrideActive{false};
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
    std::atomic<quint64> audioMonitorTrackId{0};
    std::atomic<bool> audioInputMonitoring{false};
    double audioMonitorNextSourceFrame = 0.0;
    quint64 audioMonitorSourceFrameIndex = 0;
    StereoSample audioMonitorPreviousSample;
    bool audioMonitorHasPreviousSample = false;
    size_t liveMidiTouchedTrackCount = 0;
    std::atomic<size_t> liveMidiWriteIndex{0};
    std::atomic<size_t> liveMidiReadIndex{0};
    std::atomic<bool> liveMidiPanic{false};
    std::atomic<bool> clapPreviewNoteOffPending{false};
    std::atomic<qint64> clapPreviewNoteOffFrame{-1};
    std::atomic<std::int32_t> clapPreviewNoteOffId{0};
    std::atomic<int> clapPreviewNoteOffChannel{0};
    std::atomic<int> clapPreviewNoteOffPitch{0};
    std::atomic<std::int32_t> nextClapPreviewNoteId{1};
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
    std::atomic<bool> previewSecondOscillatorEnabled{false};
    std::atomic<int> previewSecondOscillatorWaveform{static_cast<int>(SynthWaveform::Saw)};
    std::atomic<float> previewSecondOscillatorDetuneCents{0.0f};
    std::atomic<float> previewSecondOscillatorMix{0.5f};
    std::atomic<bool> previewSynthFilterEnabled{false};
    std::atomic<int> previewFilterType{static_cast<int>(SynthFilterType::LowPass)};
    std::atomic<float> previewFilterCutoffHz{20000.0f};
    std::atomic<float> previewFilterResonance{0.707f};
    std::atomic<bool> previewFilterEnvelopeEnabled{false};
    std::atomic<float> previewFilterEnvelopeAmountSemitones{24.0f};
    std::atomic<qint64> previewFilterEnvelopeAttackFrames{1};
    std::atomic<qint64> previewFilterEnvelopeDecayFrames{0};
    std::atomic<qint64> previewFilterEnvelopeReleaseFrames{1};
    std::atomic<float> previewFilterEnvelopeSustainLevel{0.0f};
    std::atomic<float> previewFilterB0{1.0f};
    std::atomic<float> previewFilterB1{0.0f};
    std::atomic<float> previewFilterB2{0.0f};
    std::atomic<float> previewFilterA1{0.0f};
    std::atomic<float> previewFilterA2{0.0f};
    std::atomic<quint64> previewFilterResetSerial{0};
    quint64 previewFilterSeenSerial = 0;
    bool previewFilterActive = false;
    float previewFilterX1 = 0.0f;
    float previewFilterX2 = 0.0f;
    float previewFilterY1 = 0.0f;
    float previewFilterY2 = 0.0f;
    float previewFilterCachedB0 = 1.0f;
    float previewFilterCachedB1 = 0.0f;
    float previewFilterCachedB2 = 0.0f;
    float previewFilterCachedA1 = 0.0f;
    float previewFilterCachedA2 = 0.0f;
    std::atomic<bool> previewPercussion{false};
    std::atomic<float> previewLeftGain{0.70710678f};
    std::atomic<float> previewRightGain{0.70710678f};
    std::atomic<qint64> previewStartFrame{0};
    std::atomic<qint64> previewNoteEndFrame{0};
    std::atomic<qint64> previewEndFrame{0};
    std::atomic<qint64> previewAttackFrames{1};
    std::atomic<qint64> previewDecayFrames{0};
    std::atomic<qint64> previewReleaseFrames{0};
    std::atomic<float> previewSustainLevel{1.0f};
    std::shared_ptr<const AudioAsset> previewSampleAsset;
    std::atomic<double> previewSampleStartFrame{0.0};
    std::atomic<qint64> previewSampleEndFrame{0};
    std::atomic<double> previewSamplePlaybackRate{1.0};
    std::atomic<bool> previewSampleLoopEnabled{false};
    std::atomic<float> previewSampleGain{1.0f};

    qint64 playbackFrame = 0;
};
