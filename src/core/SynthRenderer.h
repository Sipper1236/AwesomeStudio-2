#pragma once

#include "Lv2AudioEffect.h"
#include "Project.h"

#include <array>
#include <memory>
#include <vector>

struct StereoSample
{
    float left = 0.0f;
    float right = 0.0f;
};

float renderPercussionSample(int midiPitch, qint64 ageFrames, qint64 startFrame, double sampleRate);

class SynthRenderer
{
public:
    void setProject(std::shared_ptr<const ProjectData> project, double sampleRate);
    void resetTransport();
    void beginMixerFrame();
    void setCurrentBeat(double beat);
    StereoSample addLiveTrackSample(size_t trackIndex, StereoSample input);
    StereoSample finishMixerFrame();
    StereoSample processMasterEffects(StereoSample input);
    double effectTailSeconds() const;
    void setLiveTrackMixOverride(size_t trackIndex, int parameter, float value);
    void setLiveLv2ParameterOverride(size_t trackIndex, qsizetype effectIndex,
                                     qsizetype parameterIndex, float value);
    void resetTrackPeakLevels();
    void resetBusPeakLevels();
    float trackPeakLevel(size_t trackIndex) const;
    float busPeakLevel(size_t busIndex) const;
    StereoSample renderSample(qint64 projectFrame);

private:
    struct VoiceNote
    {
        qint64 startFrame = 0;
        qint64 endFrame = 0;
        qint64 attackFrames = 1;
        qint64 decayFrames = 0;
        qint64 releaseFrames = 1;
        size_t trackIndex = 0;
        double frequency = 440.0;
        double baseFrequency = 440.0;
        float amplitude = 0.0f;
        float sustainLevel = 1.0f;
        SynthWaveform waveform = SynthWaveform::Sine;
        SynthWaveform secondOscillatorWaveform = SynthWaveform::Saw;
        float secondOscillatorDetuneCents = 0.0f;
        float secondOscillatorMix = 0.5f;
        int midiPitch = 60;
        int midiChannel = 0;
        int probabilityPercent = 100;
        quint64 probabilitySeed = 0;
        bool percussion = false;
        bool secondOscillatorEnabled = false;
    };

    struct PitchBendEvent
    {
        qint64 frame = 0;
        size_t trackIndex = 0;
        int channel = 0;
        float semitones = 0.0f;
    };

    struct AudioRegion
    {
        qint64 startFrame = 0;
        qint64 endFrame = 0;
        qint64 sourceStartFrame = 0;
        qint64 fadeInFrames = 0;
        qint64 fadeOutFrames = 0;
        AudioFadeCurve fadeInCurve = AudioFadeCurve::Linear;
        AudioFadeCurve fadeOutCurve = AudioFadeCurve::Linear;
        double fadeInShape = 1.0;
        double fadeOutShape = 1.0;
        size_t trackIndex = 0;
        float sourceGain = 1.0f;
        double playbackRate = 1.0;
        int probabilityPercent = 100;
        quint64 probabilitySeed = 0;
        bool drumSamplePlayback = false;
        bool drumSampleLoop = false;
        qint64 drumGateEndFrame = 0;
        qint64 drumAttackFrames = 1;
        qint64 drumDecayFrames = 0;
        qint64 drumReleaseFrames = 0;
        float drumSustainLevel = 1.0f;
        std::shared_ptr<const AudioAsset> source;
    };

    struct TrackMixAutomation
    {
        float baseVolume = 1.0f;
        float basePan = 0.0f;
        QVector<AutomationPoint> volume;
        QVector<AutomationPoint> pan;
    };

    struct TrackMixValue
    {
        qint64 frame = -1;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
    };

    struct LiveTrackMixOverride
    {
        float volume = -1.0f;
        float pan = 2.0f;
    };

    struct TrackEqualizerState
    {
        float lowLeft = 0.0f;
        float lowRight = 0.0f;
        float upperLeft = 0.0f;
        float upperRight = 0.0f;
        bool tailActive = false;
    };

    struct BusMixState
    {
        float leftGain = 1.0f;
        float rightGain = 1.0f;
    };

    struct TrackSendMix
    {
        int busIndex = -1;
        float gain = 0.0f;
    };

    struct EffectState
    {
        MixerEffect settings;
        std::vector<float> delayLeft;
        std::vector<float> delayRight;
        size_t delayIndex = 0;
        std::array<std::vector<float>, 8> reverbLines;
        std::array<size_t, 8> reverbIndices{};
        std::array<float, 8> reverbDamped{};
        std::array<float, 2> filterX1{};
        std::array<float, 2> filterX2{};
        std::array<float, 2> filterY1{};
        std::array<float, 2> filterY2{};
        std::vector<float> liveParameterOverrides;
        std::unique_ptr<Lv2AudioEffectInstance> lv2Effect;
        float filterB0 = 1.0f;
        float filterB1 = 0.0f;
        float filterB2 = 0.0f;
        float filterA1 = 0.0f;
        float filterA2 = 0.0f;
        float compressorEnvelope = 0.0f;
        qint64 tailFramesRemaining = 0;
    };

    const TrackMixValue& trackMixAt(size_t trackIndex, qint64 frame, double beat);
    void addTrackSample(size_t trackIndex, float left, float right);
    void addBusFrameSample(size_t busIndex, StereoSample input);
    StereoSample applyTrackEqualizer(size_t trackIndex, StereoSample input);
    void resetTrackEqualizers();
    void prepareEffectChain(const QVector<MixerEffect>& settings, std::vector<EffectState>& states);
    void resetEffectChain(std::vector<EffectState>& states);
    bool effectChainHasTail(const std::vector<EffectState>& states) const;
    StereoSample processEffectChain(std::vector<EffectState>& states, StereoSample input);

    std::shared_ptr<const ProjectData> project;
    std::vector<TrackMixAutomation> trackMixAutomations;
    std::vector<TrackMixValue> trackMixValues;
    std::vector<LiveTrackMixOverride> liveTrackMixOverrides;
    std::vector<std::array<float, 3>> trackEqualizerGains;
    std::vector<TrackEqualizerState> trackEqualizerStates;
    std::vector<std::vector<EffectState>> trackInstrumentFilterStates;
    std::vector<unsigned char> trackInstrumentFilterHasTail;
    std::vector<std::vector<EffectState>> trackEffectStates;
    std::vector<unsigned char> trackEffectHasTail;
    std::vector<size_t> activeEqualizerTailTracks;
    std::vector<int> trackBusIndices;
    std::vector<std::vector<TrackSendMix>> trackSends;
    std::vector<unsigned char> trackAudible;
    std::vector<BusMixState> busMixStates;
    std::vector<std::vector<EffectState>> busEffectStates;
    std::vector<unsigned char> busEffectHasTail;
    std::vector<std::vector<TrackSendMix>> busSends;
    std::vector<int> busOutputIndices;
    std::vector<unsigned char> busInputAudible;
    std::vector<unsigned char> busOutputAudible;
    std::vector<unsigned char> busSoloDownstreamAudible;
    std::vector<size_t> busRouteOrder;
    std::vector<StereoSample> busFrameSamples;
    std::vector<StereoSample> lastBusOutputSamples;
    std::vector<unsigned char> busFrameTouched;
    std::vector<size_t> touchedBusIndices;
    std::vector<float> busPeakLevels;
    std::vector<StereoSample> trackFrameSamples;
    std::vector<unsigned char> trackFrameTouched;
    std::vector<size_t> touchedTrackIndices;
    std::vector<float> trackPeakLevels;
    std::vector<EffectState> masterEffectStates;
    std::vector<VoiceNote> notes;
    std::vector<PitchBendEvent> pitchBendEvents;
    std::vector<std::array<float, 16>> trackPitchBends;
    std::vector<size_t> activeNotes;
    std::vector<AudioRegion> audioRegions;
    std::vector<size_t> activeAudioRegions;
    size_t nextStartIndex = 0;
    size_t nextPitchBendIndex = 0;
    size_t nextAudioStartIndex = 0;
    quint64 playbackCycle = 0;
    qint64 lastFrame = -1;
    double sampleRate = 48000.0;
    double currentBeat = 0.0;
    float lowBandCoefficient = 0.0f;
    float highBandCoefficient = 0.0f;
    bool hasBusSolo = false;
};
