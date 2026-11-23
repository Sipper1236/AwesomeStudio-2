#include "Project.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

bool MidiNote::operator==(const MidiNote& other) const
{
    return id == other.id && pitch == other.pitch && velocity == other.velocity &&
           std::abs(startBeat - other.startBeat) < 1.0e-9 &&
           std::abs(lengthBeats - other.lengthBeats) < 1.0e-9;
}

double ProjectData::patternLengthBeats() const
{
    return quarterNotesPerBar() * bars;
}

double ProjectData::quarterNotesPerBar() const
{
    return static_cast<double>(beatsPerBar) * 4.0 / beatUnit;
}

ProjectData::ProjectData()
{
    InstrumentTrack track;
    track.id = 1;
    track.name = QStringLiteral("Synth 1");

    MidiClip clip;
    clip.id = 1;
    clip.name = QStringLiteral("Pattern 1");
    track.clips.push_back(clip);
    tracks.push_back(track);
}

double ProjectData::songLengthBeats() const
{
    double length = patternLengthBeats();
    for (const InstrumentTrack& track : tracks)
    {
        for (const MidiClip& clip : track.clips)
            length = std::max(length, clip.startBeat + clip.lengthBeats);
    }
    return length;
}

InstrumentTrack* ProjectData::findTrack(quint64 trackId)
{
    for (InstrumentTrack& track : tracks)
        if (track.id == trackId) return &track;
    return nullptr;
}

const InstrumentTrack* ProjectData::findTrack(quint64 trackId) const
{
    for (const InstrumentTrack& track : tracks)
        if (track.id == trackId) return &track;
    return nullptr;
}

MidiClip* ProjectData::findClip(quint64 trackId, quint64 clipId)
{
    InstrumentTrack* track = findTrack(trackId);
    if (track == nullptr) return nullptr;
    for (MidiClip& clip : track->clips)
        if (clip.id == clipId) return &clip;
    return nullptr;
}

const MidiClip* ProjectData::findClip(quint64 trackId, quint64 clipId) const
{
    const InstrumentTrack* track = findTrack(trackId);
    if (track == nullptr) return nullptr;
    for (const MidiClip& clip : track->clips)
        if (clip.id == clipId) return &clip;
    return nullptr;
}

quint64 ProjectData::nextTrackId() const
{
    quint64 nextId = 1;
    for (const InstrumentTrack& track : tracks)
        nextId = std::max(nextId, track.id + 1);
    return nextId;
}

quint64 ProjectData::nextClipId() const
{
    quint64 nextId = 1;
    for (const InstrumentTrack& track : tracks)
        for (const MidiClip& clip : track.clips)
            nextId = std::max(nextId, clip.id + 1);
    return nextId;
}

quint64 ProjectData::nextNoteId() const
{
    quint64 nextId = 1;
    for (const InstrumentTrack& track : tracks)
        for (const MidiClip& clip : track.clips)
            for (const MidiNote& note : clip.notes)
                nextId = std::max(nextId, note.id + 1);
    return nextId;
}

void ProjectData::normalize()
{
    tempoBpm = std::clamp(tempoBpm, 20.0, 300.0);
    beatsPerBar = std::clamp(beatsPerBar, 1, 12);
    if (beatUnit != 1 && beatUnit != 2 && beatUnit != 4 && beatUnit != 8 && beatUnit != 16)
        beatUnit = 4;
    bars = std::clamp(bars, 1, 64);
    masterGain = std::clamp(masterGain, 0.0f, 1.0f);
    if (tracks.size() > 256)
        tracks.resize(256);

    std::unordered_set<quint64> usedTrackIds;
    std::unordered_set<quint64> usedClipIds;
    quint64 trackCandidate = nextTrackId();
    quint64 clipCandidate = nextClipId();
    quint64 noteCandidate = nextNoteId();
    for (InstrumentTrack& track : tracks)
    {
        if (track.id == 0 || usedTrackIds.find(track.id) != usedTrackIds.end())
        {
            while (usedTrackIds.find(trackCandidate) != usedTrackIds.end() || trackCandidate == 0)
                ++trackCandidate;
            track.id = trackCandidate++;
        }
        usedTrackIds.insert(track.id);
        track.instrumentType = static_cast<InstrumentType>(
            std::clamp(static_cast<int>(track.instrumentType), 0, 1));
        track.gain = std::clamp(track.gain, 0.0f, 1.0f);
        track.pan = std::clamp(track.pan, -1.0f, 1.0f);
        track.waveform = static_cast<SynthWaveform>(std::clamp(static_cast<int>(track.waveform), 0, 3));
        if (track.clips.size() > 10000)
            track.clips.resize(10000);

        std::unordered_set<quint64> usedNotes;
        QVector<MidiClip> validClips;
        validClips.reserve(track.clips.size());
        for (MidiClip clip : track.clips)
        {
            if (!std::isfinite(clip.startBeat) || !std::isfinite(clip.lengthBeats))
                continue;
            if (clip.id == 0 || usedClipIds.find(clip.id) != usedClipIds.end())
            {
                while (usedClipIds.find(clipCandidate) != usedClipIds.end() || clipCandidate == 0)
                    ++clipCandidate;
                clip.id = clipCandidate++;
            }
            usedClipIds.insert(clip.id);
            clip.startBeat = std::clamp(clip.startBeat, 0.0, 16384.0);
            clip.lengthBeats = std::clamp(clip.lengthBeats, 0.25, 16384.0);
            if (clip.notes.size() > 100000)
                clip.notes.resize(100000);

            QVector<MidiNote> validNotes;
            validNotes.reserve(clip.notes.size());
            for (MidiNote note : clip.notes)
            {
                if (!std::isfinite(note.startBeat) || !std::isfinite(note.lengthBeats))
                    continue;
                note.pitch = std::clamp(note.pitch, 0, 127);
                note.velocity = std::clamp(note.velocity, 1, 127);
                const double minimumLength = 1.0 / 64.0;
                note.startBeat = std::clamp(note.startBeat, 0.0,
                                            std::max(0.0, clip.lengthBeats - minimumLength));
                note.lengthBeats = std::clamp(note.lengthBeats, minimumLength,
                                              std::max(minimumLength, clip.lengthBeats - note.startBeat));
                if (note.id == 0 || usedNotes.find(note.id) != usedNotes.end())
                {
                    while (usedNotes.find(noteCandidate) != usedNotes.end() || noteCandidate == 0)
                        ++noteCandidate;
                    note.id = noteCandidate++;
                }
                usedNotes.insert(note.id);
                validNotes.push_back(note);
            }
            std::sort(validNotes.begin(), validNotes.end(), [](const MidiNote& left, const MidiNote& right)
            {
                if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
                return left.pitch < right.pitch;
            });
            clip.notes = std::move(validNotes);
            validClips.push_back(std::move(clip));
        }

        std::sort(validClips.begin(), validClips.end(), [](const MidiClip& left, const MidiClip& right)
        {
            return left.startBeat < right.startBeat;
        });
        track.clips = std::move(validClips);
    }
}
