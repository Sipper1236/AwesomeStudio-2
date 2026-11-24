#pragma once

#include <QVector>
#include <QString>
#include <QPointF>
#include <QtGlobal>

#include <array>
#include <memory>

enum class SynthWaveform
{
    Sine,
    Triangle,
    Saw,
    Square
};

enum class SynthFilterType
{
    LowPass,
    HighPass,
    BandPass,
    Notch
};

enum class InstrumentType
{
    Synth,
    DrumRack,
    Audio,
    ClapInstrument,
    Vst3Instrument
};

enum class AudioFadeCurve
{
    Linear,
    Smooth,
    EqualPower,
    Custom,
    Drawn,
    Bezier
};

constexpr int audioFadeCurvePointCount = 33;
constexpr int audioFadeBezierHandleCount = 2;
constexpr int audioFadeBezierCurvePointCount = 129;

struct AudioAsset
{
    static constexpr qint64 waveformBlockFrames = 256;

    QString path;
    int sampleRate = 44100;
    int channelCount = 2;
    QVector<float> samples;
    QVector<float> waveformPeaks;

    qint64 frameCount() const
    {
        return channelCount > 0 ? samples.size() / channelCount : 0;
    }
};

struct MidiNote
{
    quint64 id = 0;
    int pitch = 60;
    int velocity = 100;
    double startBeat = 0.0;
    double lengthBeats = 1.0;
    int channel = -1;
    int probabilityPercent = 100;
    int ratchetCount = 1;

    bool operator==(const MidiNote& other) const;
};

struct MidiPitchBend
{
    int channel = 0;
    int value = 8192;
    double beat = 0.0;

    bool operator==(const MidiPitchBend& other) const;
};

struct MidiClip
{
    quint64 id = 0;
    QString name = QStringLiteral("Pattern");
    double startBeat = 0.0;
    double lengthBeats = 16.0;
    int stepsPerBar = 16;
    QVector<MidiNote> notes;
    QVector<MidiPitchBend> pitchBends;
    QString audioSourcePath;
    std::shared_ptr<const AudioAsset> audioSource;
    qint64 audioStartFrame = 0;
    quint64 audioTakeGroupId = 0;
    bool audioTakeActive = true;
    bool audioTakeEligible = false;
    double audioFadeInBeats = 0.0;
    double audioFadeOutBeats = 0.0;
    AudioFadeCurve audioFadeInCurve = AudioFadeCurve::Linear;
    AudioFadeCurve audioFadeOutCurve = AudioFadeCurve::Linear;
    double audioFadeInShape = 1.0;
    double audioFadeOutShape = 1.0;
    QVector<float> audioFadeInCurvePoints;
    QVector<float> audioFadeOutCurvePoints;
    QVector<QPointF> audioFadeInBezierHandles;
    QVector<QPointF> audioFadeOutBezierHandles;

    bool isAudioRegion() const { return !audioSourcePath.isEmpty(); }
};

QVector<float> defaultAudioFadeCurvePoints();
void normalizeAudioFadeCurvePoints(QVector<float>& points);
QVector<QPointF> defaultAudioFadeBezierHandles();
void normalizeAudioFadeBezierHandles(QVector<QPointF>& handles);
QVector<float> audioFadeBezierCurvePoints(const QVector<QPointF>& handles);
float audioFadeEnvelope(AudioFadeCurve curve, bool fadeIn, double progress, double shape = 1.0,
                        const QVector<float>* drawnPoints = nullptr);

struct DrumLayerSettings
{
    float tuningSemitones = 0.0f;
    float startPositionPercent = 0.0f;
    float endPositionPercent = 100.0f;
    bool loopEnabled = false;
    float attackSeconds = 0.008f;
    float decaySeconds = 0.0f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.06f;
};

struct DrumPadSample
{
    QString path;
    std::shared_ptr<const AudioAsset> source;
    QString veryLowVelocityPath;
    std::shared_ptr<const AudioAsset> veryLowVelocitySource;
    int veryLowVelocityThreshold = 24;
    QString lowVelocityPath;
    std::shared_ptr<const AudioAsset> lowVelocitySource;
    int lowVelocityThreshold = 48;
    QString highVelocityPath;
    std::shared_ptr<const AudioAsset> highVelocitySource;
    int highVelocityThreshold = 100;
    QString veryHighVelocityPath;
    std::shared_ptr<const AudioAsset> veryHighVelocitySource;
    int veryHighVelocityThreshold = 120;
    float tuningSemitones = 0.0f;
    float startPositionPercent = 0.0f;
    float endPositionPercent = 100.0f;
    bool loopEnabled = false;
    float attackSeconds = 0.008f;
    float decaySeconds = 0.0f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.06f;
    std::array<DrumLayerSettings, 5> velocityLayerSettings{};
    std::array<bool, 5> velocityLayerSettingsCustom{};

    bool hasSampleAssignment() const
    {
        return !path.isEmpty() || !veryLowVelocityPath.isEmpty() || !lowVelocityPath.isEmpty() ||
               !highVelocityPath.isEmpty() || !veryHighVelocityPath.isEmpty();
    }
    int sourceLayerIndexForVelocity(int velocity) const
    {
        const std::array<const std::shared_ptr<const AudioAsset>*, 5> layers = {
            &veryLowVelocitySource, &lowVelocitySource, &source,
            &highVelocitySource, &veryHighVelocitySource
        };
        int selected = 0;
        if (velocity >= veryLowVelocityThreshold) ++selected;
        if (velocity >= lowVelocityThreshold) ++selected;
        if (velocity >= highVelocityThreshold) ++selected;
        if (velocity >= veryHighVelocityThreshold) ++selected;
        for (int index = selected; index >= 0; --index)
            if (*layers[static_cast<size_t>(index)] != nullptr)
                return index;
        for (size_t index = static_cast<size_t>(selected + 1); index < layers.size(); ++index)
            if (*layers[index] != nullptr)
                return static_cast<int>(index);
        return -1;
    }
    const std::shared_ptr<const AudioAsset>& sourceForVelocity(int velocity) const
    {
        const std::array<const std::shared_ptr<const AudioAsset>*, 5> layers = {
            &veryLowVelocitySource, &lowVelocitySource, &source,
            &highVelocitySource, &veryHighVelocitySource
        };
        const int layer = sourceLayerIndexForVelocity(velocity);
        return layer < 0 ? source : *layers[static_cast<size_t>(layer)];
    }
    DrumLayerSettings sharedLayerSettings() const
    {
        DrumLayerSettings settings;
        settings.tuningSemitones = tuningSemitones;
        settings.startPositionPercent = startPositionPercent;
        settings.endPositionPercent = endPositionPercent;
        settings.loopEnabled = loopEnabled;
        settings.attackSeconds = attackSeconds;
        settings.decaySeconds = decaySeconds;
        settings.sustainLevel = sustainLevel;
        settings.releaseSeconds = releaseSeconds;
        return settings;
    }
    void setSharedLayerSettings(const DrumLayerSettings& settings)
    {
        tuningSemitones = settings.tuningSemitones;
        startPositionPercent = settings.startPositionPercent;
        endPositionPercent = settings.endPositionPercent;
        loopEnabled = settings.loopEnabled;
        attackSeconds = settings.attackSeconds;
        decaySeconds = settings.decaySeconds;
        sustainLevel = settings.sustainLevel;
        releaseSeconds = settings.releaseSeconds;
    }
    DrumLayerSettings layerSettings(int layer) const
    {
        if (layer < 0 || layer >= static_cast<int>(velocityLayerSettings.size()) ||
            !velocityLayerSettingsCustom[static_cast<size_t>(layer)])
            return sharedLayerSettings();
        return velocityLayerSettings[static_cast<size_t>(layer)];
    }
    DrumLayerSettings settingsForVelocity(int velocity) const
    {
        return layerSettings(sourceLayerIndexForVelocity(velocity));
    }
    void setLayerSettings(int layer, const DrumLayerSettings& settings)
    {
        if (layer < 0 || layer >= static_cast<int>(velocityLayerSettings.size()))
            return;
        velocityLayerSettings[static_cast<size_t>(layer)] = settings;
        velocityLayerSettingsCustom[static_cast<size_t>(layer)] = true;
    }
    bool hasLayerAssignment(int layer) const
    {
        switch (layer)
        {
            case 0: return !veryLowVelocityPath.isEmpty();
            case 1: return !lowVelocityPath.isEmpty();
            case 2: return !path.isEmpty();
            case 3: return !highVelocityPath.isEmpty();
            case 4: return !veryHighVelocityPath.isEmpty();
            default: return false;
        }
    }
};

struct AutomationPoint
{
    double beat = 0.0;
    float value = 0.0f;
    enum class Curve
    {
        Linear,
        Smooth,
        EaseIn,
        EaseOut,
        Hold
    } curveToNext = Curve::Linear;
};

inline double automationCurveProgress(AutomationPoint::Curve curve, double position)
{
    const double t = position <= 0.0 ? 0.0 : position >= 1.0 ? 1.0 : position;
    switch (curve)
    {
        case AutomationPoint::Curve::Smooth:
            return t * t * (3.0 - 2.0 * t);
        case AutomationPoint::Curve::EaseIn:
            return t * t;
        case AutomationPoint::Curve::EaseOut:
        {
            const double inverse = 1.0 - t;
            return 1.0 - inverse * inverse;
        }
        case AutomationPoint::Curve::Hold:
            return t < 1.0 ? 0.0 : 1.0;
        case AutomationPoint::Curve::Linear:
        default:
            return t;
    }
}

struct TimelineMarker
{
    double beat = 0.0;
    QString name = QStringLiteral("Marker");

    bool operator==(const TimelineMarker& other) const;
};

struct TempoChange
{
    double beat = 0.0;
    double bpm = 120.0;
    bool rampFromPrevious = false;

    bool operator==(const TempoChange& other) const;
};

struct MixerSend
{
    quint64 busId = 0;
    float gain = 0.0f;
};

enum class MixerEffectType
{
    Delay,
    Reverb,
    Compressor,
    LowPass,
    Lv2Plugin,
    ClapPlugin,
    Vst3Plugin
};

inline bool isPluginEffect(MixerEffectType type)
{
    return type == MixerEffectType::Lv2Plugin || type == MixerEffectType::ClapPlugin ||
           type == MixerEffectType::Vst3Plugin;
}

inline constexpr qsizetype maximumPluginStateBytes = 1024 * 1024;

struct MixerEffect
{
    MixerEffectType type = MixerEffectType::Delay;
    bool enabled = true;
    float wet = 0.25f;
    float delayMs = 250.0f;
    float feedback = 0.3f;
    float roomSize = 0.6f;
    float damping = 0.5f;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float makeupDb = 0.0f;
    float cutoffHz = 20000.0f;
    float resonance = 0.707f;
    QString pluginUri;
    QString pluginName;
    QVector<float> pluginParameterValues;
    QVector<QVector<AutomationPoint>> pluginParameterAutomation;
    QString pluginState;
};

MixerEffect defaultMixerEffect(MixerEffectType type);

struct MixerBus
{
    quint64 id = 0;
    QString name = QStringLiteral("Bus");
    quint64 outputBusId = 0;
    QVector<MixerSend> sends;
    QVector<MixerEffect> effects;
    float gain = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    bool solo = false;
};

struct ParametricEqBand
{
    float frequencyHz = 1000.0f;
    float gainDb = 0.0f;
    float q = 0.707f;
};

constexpr int parametricEqBandCount = 7;

struct InstrumentTrack
{
    quint64 id = 0;
    QString name = QStringLiteral("Synth");
    InstrumentType instrumentType = InstrumentType::Synth;
    QString clapPluginId;
    QString clapPluginName;
    QString clapPluginState;
    QVector<float> clapParameterValues;
    QVector<QVector<AutomationPoint>> clapParameterAutomation;
    QString vst3PluginId;
    QString vst3PluginName;
    QString vst3PluginState;
    QVector<float> vst3ParameterValues;
    QVector<QVector<AutomationPoint>> vst3ParameterAutomation;
    SynthWaveform waveform = SynthWaveform::Sine;
    bool secondOscillatorEnabled = false;
    SynthWaveform secondOscillatorWaveform = SynthWaveform::Saw;
    float secondOscillatorDetuneCents = 0.0f;
    float secondOscillatorMix = 0.5f;
    bool synthFilterEnabled = false;
    SynthFilterType synthFilterType = SynthFilterType::LowPass;
    float synthFilterCutoffHz = 20000.0f;
    float synthFilterResonance = 0.707f;
    bool synthFilterEnvelopeEnabled = false;
    float synthFilterEnvelopeAmountSemitones = 24.0f;
    float synthFilterEnvelopeAttackSeconds = 0.008f;
    float synthFilterEnvelopeDecaySeconds = 0.2f;
    float synthFilterEnvelopeSustainLevel = 0.0f;
    float synthFilterEnvelopeReleaseSeconds = 0.2f;
    float attackSeconds = 0.008f;
    float decaySeconds = 0.0f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.06f;
    float gain = 0.8f;
    float pan = 0.0f;
    float eqLowDb = 0.0f;
    float eqMidDb = 0.0f;
    float eqHighDb = 0.0f;
    float eqLowCrossoverHz = 120.0f;
    float eqHighCrossoverHz = 6000.0f;
    std::array<ParametricEqBand, parametricEqBandCount> eqParametricBands{{
        {120.0f, 0.0f, 0.707f}, {1000.0f, 0.0f, 0.707f}, {8000.0f, 0.0f, 0.707f},
        {40.0f, 0.0f, 0.707f}, {350.0f, 0.0f, 0.707f},
        {3000.0f, 0.0f, 0.707f}, {16000.0f, 0.0f, 0.707f}
    }};
    quint64 outputBusId = 0;
    QVector<MixerSend> sends;
    QVector<MixerEffect> effects;
    QVector<AutomationPoint> volumeAutomation;
    QVector<AutomationPoint> panAutomation;
    QVector<AutomationPoint> eqLowGainAutomation;
    QVector<AutomationPoint> eqMidGainAutomation;
    QVector<AutomationPoint> eqHighGainAutomation;
    QVector<AutomationPoint> eqLowCrossoverAutomation;
    QVector<AutomationPoint> eqHighCrossoverAutomation;
    QVector<AutomationPoint> synthFilterCutoffAutomation;
    QVector<AutomationPoint> synthFilterResonanceAutomation;
    bool muted = false;
    bool solo = false;
    std::array<DrumPadSample, 4> drumSamples;
    QVector<MidiClip> clips;
};

struct ProjectData
{
    ProjectData();

    QString title = QStringLiteral("Untitled");
    double tempoBpm = 120.0;
    int beatsPerBar = 4;
    int beatUnit = 4;
    int bars = 4;
    bool loopEnabled = true;
    bool metronomeEnabled = false;
    int countInBars = 0;
    float masterGain = 0.8f;
    double pitchBendRangeSemitones = 2.0;
    QVector<MixerEffect> masterEffects;
    QVector<TimelineMarker> markers;
    QVector<TempoChange> tempoChanges;
    QVector<MixerBus> buses;
    QVector<InstrumentTrack> tracks;

    double patternLengthBeats() const;
    double quarterNotesPerBar() const;
    double songLengthBeats() const;
    double tempoAtBeat(double beat) const;
    double secondsAtBeat(double beat) const;
    double beatAtSeconds(double seconds) const;
    qint64 frameAtBeat(double beat, int sampleRate) const;
    double beatAtFrame(qint64 frame, int sampleRate) const;
    qint64 framesBetweenBeats(double startBeat, double endBeat, int sampleRate) const;
    void rebuildTempoCache();
    InstrumentTrack* findTrack(quint64 trackId);
    const InstrumentTrack* findTrack(quint64 trackId) const;
    MidiClip* findClip(quint64 trackId, quint64 clipId);
    const MidiClip* findClip(quint64 trackId, quint64 clipId) const;
    quint64 nextTrackId() const;
    quint64 nextBusId() const;
    quint64 nextClipId() const;
    quint64 nextNoteId() const;
    void normalize();

private:
    QVector<double> tempoChangeElapsedSeconds;
};
