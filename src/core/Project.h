#pragma once

#include <QVector>
#include <QString>
#include <QtGlobal>

enum class SynthWaveform
{
    Sine,
    Triangle,
    Saw,
    Square
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
};

struct InstrumentTrack
{
    quint64 id = 0;
    QString name = QStringLiteral("Synth");
    SynthWaveform waveform = SynthWaveform::Sine;
    float gain = 0.8f;
    float pan = 0.0f;
    bool muted = false;
    bool solo = false;
    QVector<MidiClip> clips;
};

struct ProjectData
{
    ProjectData();

    QString title = QStringLiteral("Untitled");
    double tempoBpm = 120.0;
    int beatsPerBar = 4;
    int bars = 4;
    bool loopEnabled = true;
    float masterGain = 0.8f;
    QVector<InstrumentTrack> tracks;

    double patternLengthBeats() const;
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
