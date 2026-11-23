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

    bool operator==(const MidiNote& other) const;
};

struct MidiClip
{
    quint64 id = 0;
    QString name = QStringLiteral("Pattern");
    double startBeat = 0.0;
    double lengthBeats = 16.0;
    QVector<MidiNote> notes;
    QString audioSourcePath;
    std::shared_ptr<const AudioAsset> audioSource;
    qint64 audioStartFrame = 0;
    double audioFadeInBeats = 0.0;
    double audioFadeOutBeats = 0.0;

    bool isAudioRegion() const { return !audioSourcePath.isEmpty(); }
};

struct DrumPadSample
{
    QString path;
    std::shared_ptr<const AudioAsset> source;
};

struct InstrumentTrack
{
    quint64 id = 0;
    QString name = QStringLiteral("Synth");
    InstrumentType instrumentType = InstrumentType::Synth;
    SynthWaveform waveform = SynthWaveform::Sine;
    float gain = 0.8f;
    float pan = 0.0f;
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
    QVector<InstrumentTrack> tracks;

    double patternLengthBeats() const;
    double quarterNotesPerBar() const;
    double songLengthBeats() const;
    InstrumentTrack* findTrack(quint64 trackId);
    const InstrumentTrack* findTrack(quint64 trackId) const;
    MidiClip* findClip(quint64 trackId, quint64 clipId);
    const MidiClip* findClip(quint64 trackId, quint64 clipId) const;
    quint64 nextTrackId() const;
    quint64 nextClipId() const;
    quint64 nextNoteId() const;
    void normalize();
};
