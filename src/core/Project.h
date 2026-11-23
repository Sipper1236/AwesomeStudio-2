#pragma once

#include <QVector>
#include <QString>
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

enum class InstrumentType
{
    Synth,
    DrumRack,
    Audio
};

enum class AudioFadeCurve
{
    Linear,
    Smooth,
    EqualPower
};

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
    double audioFadeInBeats = 0.0;
    double audioFadeOutBeats = 0.0;
    AudioFadeCurve audioFadeInCurve = AudioFadeCurve::Linear;
    AudioFadeCurve audioFadeOutCurve = AudioFadeCurve::Linear;

    bool isAudioRegion() const { return !audioSourcePath.isEmpty(); }
};

float audioFadeEnvelope(AudioFadeCurve curve, bool fadeIn, double progress);

struct DrumPadSample
{
    QString path;
    std::shared_ptr<const AudioAsset> source;
    float tuningSemitones = 0.0f;
    float startPositionPercent = 0.0f;
};

struct AutomationPoint
{
    double beat = 0.0;
    float value = 0.0f;
};

struct TimelineMarker
{
    double beat = 0.0;
    QString name = QStringLiteral("Marker");

    bool operator==(const TimelineMarker& other) const;
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
    LowPass
};

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

struct InstrumentTrack
{
    quint64 id = 0;
    QString name = QStringLiteral("Synth");
    InstrumentType instrumentType = InstrumentType::Synth;
    SynthWaveform waveform = SynthWaveform::Sine;
    bool secondOscillatorEnabled = false;
    SynthWaveform secondOscillatorWaveform = SynthWaveform::Saw;
    float secondOscillatorDetuneCents = 0.0f;
    float secondOscillatorMix = 0.5f;
    bool synthFilterEnabled = false;
    float synthFilterCutoffHz = 20000.0f;
    float synthFilterResonance = 0.707f;
    float attackSeconds = 0.008f;
    float decaySeconds = 0.0f;
    float sustainLevel = 1.0f;
    float releaseSeconds = 0.06f;
    float gain = 0.8f;
    float pan = 0.0f;
    float eqLowDb = 0.0f;
    float eqMidDb = 0.0f;
    float eqHighDb = 0.0f;
    quint64 outputBusId = 0;
    QVector<MixerSend> sends;
    QVector<MixerEffect> effects;
    QVector<AutomationPoint> volumeAutomation;
    QVector<AutomationPoint> panAutomation;
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
    QVector<MixerBus> buses;
    QVector<InstrumentTrack> tracks;

    double patternLengthBeats() const;
    double quarterNotesPerBar() const;
    double songLengthBeats() const;
    InstrumentTrack* findTrack(quint64 trackId);
    const InstrumentTrack* findTrack(quint64 trackId) const;
    MidiClip* findClip(quint64 trackId, quint64 clipId);
    const MidiClip* findClip(quint64 trackId, quint64 clipId) const;
    quint64 nextTrackId() const;
    quint64 nextBusId() const;
    quint64 nextClipId() const;
    quint64 nextNoteId() const;
    void normalize();
};
