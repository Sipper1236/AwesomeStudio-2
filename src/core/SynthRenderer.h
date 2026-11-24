#pragma once

#include "ClapAudioEffect.h"
#include "Lv2AudioEffect.h"
#include "Project.h"
#include "Vst3AudioEffect.h"

#include <algorithm>
#include <array>
#include <cstdint>
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
    void setTransportContext(qint64 projectFrame, bool playing, bool cycleActive);
    StereoSample addLiveTrackSample(size_t trackIndex, StereoSample input);
    void setSynthFilterEnvelope(size_t trackIndex, float semitoneOffset);
    bool isPluginInstrumentTrack(size_t trackIndex) const;
    void pluginInstrumentNoteOn(size_t trackIndex, std::int32_t noteId,
                                int channel, int key, float velocity);
    void pluginInstrumentNoteOff(size_t trackIndex, std::int32_t noteId,
                                 int channel, int key, float velocity = 0.0f);
    void pluginInstrumentAllNotesOff(size_t trackIndex);
    void pluginInstrumentPitchBend(size_t trackIndex, int channel, int value);
    void pluginInstrumentsAllNotesOff();
    StereoSample finishMixerFrame();
    StereoSample processMasterEffects(StereoSample input);
    double effectTailSeconds() const;
    void setLiveTrackMixOverride(size_t trackIndex, int parameter, float value);
    void setLiveSynthFilterOverride(size_t trackIndex, int parameter, float value);
    void setLivePluginParameterOverride(size_t trackIndex, qsizetype effectIndex,
                                        qsizetype parameterIndex, float value);
    void setLiveVst3InstrumentParameterOverride(size_t trackIndex,
                                               qsizetype parameterIndex, float value);
    void capturePluginStates(ProjectData& target) const;
    bool hasPendingLv2LatencyChange() const;
    bool serviceLv2LatencyChanges();
    bool hasPendingClapRestart() const;
    bool serviceClapRestarts();
    bool hasPendingVst3LatencyRestart() const;
    void servicePluginCallbacks();
    void serviceVst3LatencyRestarts();
    void stopPluginProcessing();
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
        qint64 filterEnvelopeAttackFrames = 1;
        qint64 filterEnvelopeDecayFrames = 0;
        qint64 filterEnvelopeReleaseFrames = 1;
        float filterEnvelopeAmountSemitones = 24.0f;
        float filterEnvelopeSustainLevel = 0.0f;
        int midiPitch = 60;
        int midiChannel = 0;
        int probabilityPercent = 100;
        quint64 probabilitySeed = 0;
        bool percussion = false;
        bool secondOscillatorEnabled = false;
        bool filterEnvelopeEnabled = false;
    };

    struct PitchBendEvent
    {
        qint64 frame = 0;
        size_t trackIndex = 0;
        int channel = 0;
        int value = 8192;
        float semitones = 0.0f;
    };

    struct PluginInstrumentEvent
    {
        enum class Type { NoteOn, NoteOff } type = Type::NoteOn;
        qint64 frame = 0;
        qint64 endFrame = 0;
        size_t trackIndex = 0;
        std::int32_t noteId = 0;
        int channel = 0;
        int key = 60;
        float velocity = 1.0f;
        int probabilityPercent = 100;
        quint64 probabilitySeed = 0;
    };

    struct AudioRegion
    {
        qint64 startFrame = 0;
        qint64 endFrame = 0;
        qint64 sourceStartFrame = 0;
        qint64 sourceEndFrame = -1;
        qint64 fadeInFrames = 0;
        qint64 fadeOutFrames = 0;
        AudioFadeCurve fadeInCurve = AudioFadeCurve::Linear;
        AudioFadeCurve fadeOutCurve = AudioFadeCurve::Linear;
        double fadeInShape = 1.0;
        double fadeOutShape = 1.0;
        QVector<float> fadeInCurvePoints;
        QVector<float> fadeOutCurvePoints;
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
        std::array<float, 3> baseEqualizerDb{};
        QVector<AutomationPoint> volume;
        QVector<AutomationPoint> pan;
        std::array<QVector<AutomationPoint>, 3> equalizerGain;
    };

    struct TrackMixValue
    {
        qint64 frame = -1;
        float leftGain = 0.70710678f;
        float rightGain = 0.70710678f;
        std::array<float, 3> equalizerGains{1.0f, 1.0f, 1.0f};
    };

    struct LiveTrackMixOverride
    {
        float volume = -1.0f;
        float pan = 2.0f;
    };

    struct ParametricEqCoefficients
    {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        bool active = false;
    };

    struct ParametricEqDelay
    {
        float first = 0.0f;
        float second = 0.0f;
    };

    struct TrackEqualizerState
    {
        float lowLeft = 0.0f;
        float lowRight = 0.0f;
        float upperLeft = 0.0f;
        float upperRight = 0.0f;
        float lowBandCoefficient = 0.0f;
        float highBandCoefficient = 0.0f;
        std::array<ParametricEqCoefficients, parametricEqBandCount> parametricCoefficients{};
        std::array<std::array<ParametricEqDelay, 2>, 3> parametricDelays{};
        bool tailActive = false;
    };

    struct BusMixState
    {
        float leftGain = 1.0f;
        float rightGain = 1.0f;
    };

    struct LatencyDelayLine
    {
        std::vector<StereoSample> samples;
        size_t readIndex = 0;
        size_t pendingFrames = 0;

        void configure(qint64 frames)
        {
            const size_t delayFrames = frames > 0 ? static_cast<size_t>(frames) : 0;
            samples.assign(delayFrames, StereoSample{});
            readIndex = 0;
            pendingFrames = 0;
        }

        void reset()
        {
            std::fill(samples.begin(), samples.end(), StereoSample{});
            readIndex = 0;
            pendingFrames = 0;
        }

        StereoSample process(StereoSample input)
        {
            if (samples.empty())
                return input;
            const StereoSample output = samples[readIndex];
            samples[readIndex] = input;
            readIndex = (readIndex + 1) % samples.size();
            if (input.left != 0.0f || input.right != 0.0f)
                pendingFrames = samples.size();
            else if (pendingFrames > 0)
                --pendingFrames;
            return output;
        }

        bool hasPending() const { return pendingFrames > 0; }
    };

    struct TrackSendMix
    {
        int busIndex = -1;
        float gain = 0.0f;
        LatencyDelayLine latencyDelay;
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
        std::unique_ptr<ClapAudioPluginInstance> clapEffect;
        std::unique_ptr<Vst3AudioEffectInstance> vst3Effect;
        float filterB0 = 1.0f;
        float filterB1 = 0.0f;
        float filterB2 = 0.0f;
        float filterA1 = 0.0f;
        float filterA2 = 0.0f;
        float compressorEnvelope = 0.0f;
        qint64 tailFramesRemaining = 0;
    };

    const TrackMixValue& trackMixAt(size_t trackIndex, qint64 frame, double beat);
    void addTrackSample(size_t trackIndex, float left, float right,
                        bool pluginInstrumentOutput = false);
    void addBusFrameSample(size_t busIndex, StereoSample input);
    StereoSample applyTrackEqualizer(size_t trackIndex, StereoSample input);
    void resetTrackEqualizers();
    void resetLatencyCompensation();
    void recomputeLatencyCompensation();
    static ParametricEqCoefficients makeParametricEqCoefficients(
        const ParametricEqBand& band, double sampleRate);
    void prepareEffectChain(const QVector<MixerEffect>& settings, std::vector<EffectState>& states);
    Vst3TransportContext currentVst3TransportContext() const;
    void configureFilter(EffectState& state, SynthFilterType type);
    void resetEffectChain(std::vector<EffectState>& states);
    bool effectChainHasTail(const std::vector<EffectState>& states) const;
    void captureEffectStates(const std::vector<EffectState>& states,
                             QVector<MixerEffect>& target) const;
    void serviceEffectCallbacks(std::vector<EffectState>& states);
    StereoSample processEffectChain(std::vector<EffectState>& states, StereoSample input);

    std::shared_ptr<const ProjectData> project;
    std::vector<TrackMixAutomation> trackMixAutomations;
    std::vector<TrackMixValue> trackMixValues;
    std::vector<LiveTrackMixOverride> liveTrackMixOverrides;
    std::vector<float> liveSynthFilterCutoffOverrides;
    std::vector<float> liveSynthFilterResonanceOverrides;
    std::vector<TrackEqualizerState> trackEqualizerStates;
    std::vector<std::vector<EffectState>> trackInstrumentFilterStates;
    std::vector<unsigned char> trackInstrumentFilterHasTail;
    std::vector<float> synthFilterEnvelopeSemitoneOffsets;
    std::vector<float> synthFilterEnvelopeAppliedOffsets;
    quint64 mixerFrameNumber = 0;
    std::vector<std::vector<EffectState>> trackEffectStates;
    std::vector<std::unique_ptr<ClapAudioPluginInstance>> clapInstruments;
    std::vector<std::unique_ptr<Vst3AudioInstrumentInstance>> vst3Instruments;
    std::vector<std::vector<float>> pluginInstrumentLiveParameterOverrides;
    std::vector<std::vector<float>> vst3InstrumentManualParameterOverrides;
    std::vector<unsigned char> trackEffectHasTail;
    std::vector<size_t> activeEqualizerTailTracks;
    std::vector<int> trackBusIndices;
    std::vector<LatencyDelayLine> trackMainLatencyDelays;
    std::vector<LatencyDelayLine> trackInstrumentSourceLatencyDelays;
    std::vector<std::vector<TrackSendMix>> trackSends;
    std::vector<unsigned char> trackAudible;
    std::vector<BusMixState> busMixStates;
    std::vector<std::vector<EffectState>> busEffectStates;
    std::vector<unsigned char> busEffectHasTail;
    std::vector<std::vector<TrackSendMix>> busSends;
    std::vector<int> busOutputIndices;
    std::vector<LatencyDelayLine> busMainLatencyDelays;
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
    std::vector<StereoSample> trackFramePluginInstrumentSamples;
    std::vector<unsigned char> trackFrameTouched;
    std::vector<size_t> touchedTrackIndices;
    std::vector<float> trackPeakLevels;
    std::vector<EffectState> masterEffectStates;
    std::vector<VoiceNote> notes;
    std::vector<PitchBendEvent> pitchBendEvents;
    std::vector<PluginInstrumentEvent> pluginInstrumentEvents;
    std::vector<std::array<float, 16>> trackPitchBends;
    std::vector<size_t> activeNotes;
    std::vector<AudioRegion> audioRegions;
    std::vector<size_t> activeAudioRegions;
    size_t nextStartIndex = 0;
    size_t nextPitchBendIndex = 0;
    size_t nextPluginInstrumentEventIndex = 0;
    size_t nextAudioStartIndex = 0;
    quint64 playbackCycle = 0;
    qint64 lastFrame = -1;
    std::int32_t nextPluginArrangementNoteId = 1;
    double sampleRate = 48000.0;
    double currentBeat = 0.0;
    double transportProjectBeat = 0.0;
    qint64 currentProjectFrame = 0;
    bool transportPlaying = true;
    bool transportCycleActive = false;
    bool hasBusSolo = false;
};
