#include "MidiFile.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <utility>

namespace
{
constexpr qint64 maximumFileBytes = 64 * 1024 * 1024;
constexpr double maximumSongBeats = 16384.0;
constexpr int exportPpqn = 960;

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

quint8 byteAt(const QByteArray& bytes, int position)
{
    return static_cast<quint8>(static_cast<unsigned char>(bytes.at(position)));
}

quint16 readBigEndian16(const QByteArray& bytes, int offset)
{
    return static_cast<quint16>((byteAt(bytes, offset) << 8) | byteAt(bytes, offset + 1));
}

quint32 readBigEndian32(const QByteArray& bytes, int offset)
{
    return (static_cast<quint32>(byteAt(bytes, offset)) << 24) |
           (static_cast<quint32>(byteAt(bytes, offset + 1)) << 16) |
           (static_cast<quint32>(byteAt(bytes, offset + 2)) << 8) |
           static_cast<quint32>(byteAt(bytes, offset + 3));
}

bool readByte(const QByteArray& bytes, int& position, int end, quint8& value)
{
    if (position >= end)
        return false;
    value = byteAt(bytes, position++);
    return true;
}

bool readVariableLength(const QByteArray& bytes, int& position, int end, quint32& value)
{
    value = 0;
    for (int index = 0; index < 4; ++index)
    {
        quint8 part = 0;
        if (!readByte(bytes, position, end, part))
            return false;
        value = (value << 7) | (part & 0x7f);
        if ((part & 0x80) == 0)
            return true;
    }
    return false;
}

void appendByte(QByteArray& bytes, quint8 value)
{
    bytes.append(static_cast<char>(value));
}

void appendBigEndian16(QByteArray& bytes, quint16 value)
{
    appendByte(bytes, static_cast<quint8>((value >> 8) & 0xff));
    appendByte(bytes, static_cast<quint8>(value & 0xff));
}

void appendBigEndian32(QByteArray& bytes, quint32 value)
{
    appendByte(bytes, static_cast<quint8>((value >> 24) & 0xff));
    appendByte(bytes, static_cast<quint8>((value >> 16) & 0xff));
    appendByte(bytes, static_cast<quint8>((value >> 8) & 0xff));
    appendByte(bytes, static_cast<quint8>(value & 0xff));
}

void appendVariableLength(QByteArray& bytes, quint32 value)
{
    value = std::min(value, 0x0fffffffu);
    quint8 buffer[4] = {};
    int count = 1;
    buffer[3] = static_cast<quint8>(value & 0x7f);
    while ((value >>= 7) != 0 && count < 4)
        buffer[3 - count++] = static_cast<quint8>((value & 0x7f) | 0x80);
    for (int index = 4 - count; index < 4; ++index)
        appendByte(bytes, buffer[index]);
}

void appendMeta(QByteArray& track, quint8 type, const QByteArray& payload)
{
    appendByte(track, 0xff);
    appendByte(track, type);
    appendVariableLength(track, static_cast<quint32>(payload.size()));
    track.append(payload);
}

void appendTrackChunk(QByteArray& file, const QByteArray& track)
{
    file.append("MTrk", 4);
    appendBigEndian32(file, static_cast<quint32>(track.size()));
    file.append(track);
}

bool validChunk(const QByteArray& bytes, int offset, const char* id)
{
    return offset >= 0 && offset + 8 <= bytes.size() && bytes.mid(offset, 4) == QByteArray(id, 4);
}

struct RawNote
{
    qint64 startTick = 0;
    qint64 endTick = 0;
    int pitch = 60;
    int velocity = 100;
    int channel = 0;
};

struct RawPitchBend
{
    qint64 tick = 0;
    int value = 8192;
    int channel = 0;
};

struct NoteOn
{
    qint64 tick = 0;
    int velocity = 100;
    int channel = 0;
};

struct ParsedTrack
{
    QString name;
    QVector<RawNote> notes;
    QVector<RawPitchBend> pitchBends;
    double pitchBendRangeSemitones = -1.0;
    qint64 lastTick = 0;
    bool percussion = false;
};

bool parseTrack(const QByteArray& bytes, int start, int length, int ppqn,
                ParsedTrack& track, double& tempo, int& beatsPerBar, int& beatUnit,
                bool& foundTempo, bool& foundTimeSignature,
                QVector<TempoChange>& tempoChanges, QString* error)
{
    const int end = start + length;
    int position = start;
    quint8 runningStatus = 0;
    qint64 absoluteTick = 0;
    std::map<std::pair<int, int>, std::deque<NoteOn>> activeNotes;
    std::array<int, 16> rpnMsb;
    std::array<int, 16> rpnLsb;
    std::array<int, 16> bendRangeSemitones{};
    std::array<int, 16> bendRangeCents{};
    rpnMsb.fill(-1);
    rpnLsb.fill(-1);

    while (position < end)
    {
        quint32 delta = 0;
        if (!readVariableLength(bytes, position, end, delta) ||
            absoluteTick > std::numeric_limits<qint64>::max() - delta)
        {
            setError(error, QStringLiteral("The MIDI file contains an invalid event delta."));
            return false;
        }
        absoluteTick += delta;

        quint8 first = 0;
        if (!readByte(bytes, position, end, first))
        {
            setError(error, QStringLiteral("The MIDI file ends inside an event."));
            return false;
        }

        quint8 status = first;
        quint8 firstData = 0;
        bool hasFirstData = false;
        if (first < 0x80)
        {
            if (runningStatus < 0x80 || runningStatus >= 0xf0)
            {
                setError(error, QStringLiteral("The MIDI file uses running status without a channel event."));
                return false;
            }
            status = runningStatus;
            firstData = first;
            hasFirstData = true;
        }
        else if (first < 0xf0)
        {
            runningStatus = first;
        }

        if (status == 0xff)
        {
            runningStatus = 0;
            quint8 type = 0;
            quint32 size = 0;
            if (!readByte(bytes, position, end, type) ||
                !readVariableLength(bytes, position, end, size) ||
                size > static_cast<quint32>(end - position))
            {
                setError(error, QStringLiteral("The MIDI file contains a malformed metadata event."));
                return false;
            }
            const QByteArray payload = bytes.mid(position, static_cast<int>(size));
            position += static_cast<int>(size);
            if (type == 0x03 && track.name.isEmpty())
            {
                track.name = QString::fromUtf8(payload.constData(), payload.size()).trimmed();
            }
            else if (type == 0x51 && payload.size() == 3)
            {
                const quint32 microseconds =
                    (static_cast<quint32>(static_cast<quint8>(payload[0])) << 16) |
                    (static_cast<quint32>(static_cast<quint8>(payload[1])) << 8) |
                    static_cast<quint32>(static_cast<quint8>(payload[2]));
                if (microseconds != 0)
                {
                    const double bpm = 60000000.0 / microseconds;
                    const double beat = static_cast<double>(absoluteTick) / ppqn;
                    if (beat > maximumSongBeats || tempoChanges.size() >= 1000)
                    {
                        setError(error, QStringLiteral("The MIDI tempo map exceeds the project limits."));
                        return false;
                    }
                    tempoChanges.push_back({beat, bpm});
                    if (absoluteTick == 0 && !foundTempo)
                    {
                        tempo = bpm;
                        foundTempo = true;
                    }
                }
            }
            else if (type == 0x58 && payload.size() >= 2 && !foundTimeSignature)
            {
                const int numerator = static_cast<quint8>(payload[0]);
                const int exponent = static_cast<quint8>(payload[1]);
                if (numerator >= 1 && numerator <= 12 && exponent <= 4)
                {
                    beatsPerBar = numerator;
                    beatUnit = 1 << exponent;
                    foundTimeSignature = true;
                }
            }
            if (type == 0x2f)
            {
                track.lastTick = absoluteTick;
                break;
            }
            continue;
        }

        if (status == 0xf0 || status == 0xf7)
        {
            runningStatus = 0;
            quint32 size = 0;
            if (!readVariableLength(bytes, position, end, size) ||
                size > static_cast<quint32>(end - position))
            {
                setError(error, QStringLiteral("The MIDI file contains malformed system-exclusive data."));
                return false;
            }
            position += static_cast<int>(size);
            continue;
        }

        if (status >= 0xf0)
        {
            // System-common and real-time events carry no musical note data here.
            // Skip their specified data bytes while preserving channel running status
            // across real-time messages as required by MIDI.
            int dataCount = 0;
            if (status == 0xf1 || status == 0xf3) dataCount = 1;
            else if (status == 0xf2) dataCount = 2;
            if (status < 0xf8)
                runningStatus = 0;
            if (position + dataCount > end)
            {
                setError(error, QStringLiteral("The MIDI file ends inside a system event."));
                return false;
            }
            position += dataCount;
            continue;
        }

        const int eventType = status & 0xf0;
        const int channel = status & 0x0f;
        const int dataCount = (eventType == 0xc0 || eventType == 0xd0) ? 1 : 2;
        quint8 data1 = firstData;
        quint8 data2 = 0;
        if ((!hasFirstData && !readByte(bytes, position, end, data1)) ||
            (dataCount == 2 && !readByte(bytes, position, end, data2)))
        {
            setError(error, QStringLiteral("The MIDI file ends inside a channel event."));
            return false;
        }
        if (data1 >= 0x80 || data2 >= 0x80)
        {
            setError(error, QStringLiteral("The MIDI file contains invalid channel data."));
            return false;
        }

        if (eventType == 0x90 && dataCount == 2 && data2 > 0)
        {
            track.percussion = track.percussion || channel == 9;
            activeNotes[{channel, data1}].push_back({absoluteTick, data2, channel});
        }
        else if (eventType == 0x80 || (eventType == 0x90 && dataCount == 2 && data2 == 0))
        {
            auto found = activeNotes.find({channel, data1});
            if (found != activeNotes.end() && !found->second.empty())
            {
                const NoteOn noteOn = found->second.front();
                found->second.pop_front();
                track.notes.push_back({noteOn.tick, std::max(noteOn.tick + 1, absoluteTick),
                                       data1, noteOn.velocity, noteOn.channel});
            }
        }
        else if (eventType == 0xe0 && dataCount == 2)
        {
            track.pitchBends.push_back({absoluteTick,
                (static_cast<int>(data2) << 7) | static_cast<int>(data1), channel});
        }
        else if (eventType == 0xb0 && dataCount == 2)
        {
            if (data1 == 101)
                rpnMsb[static_cast<size_t>(channel)] = data2;
            else if (data1 == 100)
                rpnLsb[static_cast<size_t>(channel)] = data2;
            else if (rpnMsb[static_cast<size_t>(channel)] == 0 &&
                     rpnLsb[static_cast<size_t>(channel)] == 0 &&
                     (data1 == 6 || data1 == 38))
            {
                bool validRangeUpdate = true;
                if (data1 == 6)
                    bendRangeSemitones[static_cast<size_t>(channel)] = data2;
                else if (data2 < 100)
                    bendRangeCents[static_cast<size_t>(channel)] = data2;
                else
                    validRangeUpdate = false;
                if (validRangeUpdate)
                {
                    track.pitchBendRangeSemitones =
                        bendRangeSemitones[static_cast<size_t>(channel)] +
                        bendRangeCents[static_cast<size_t>(channel)] / 100.0;
                }
            }
        }
        track.lastTick = absoluteTick;
    }

    for (const auto& entry : activeNotes)
    {
        for (const NoteOn& noteOn : entry.second)
        {
            const qint64 defaultEnd = noteOn.tick + std::max(1, ppqn / 4);
            track.notes.push_back({noteOn.tick, std::max(defaultEnd, track.lastTick),
                                   entry.first.second, noteOn.velocity, noteOn.channel});
        }
    }
    return true;
}

struct ExportEvent
{
    qint64 tick = 0;
    int priority = 0;
    int pitch = 60;
    int velocity = 0;
    bool noteOn = false;
    int channel = 0;
    bool pitchBend = false;
    bool controlChange = false;
};

QByteArray makeConductorTrack(const ProjectData& project, qint64 songEndTick)
{
    QByteArray track;
    appendVariableLength(track, 0);
    appendMeta(track, 0x03, QByteArrayLiteral("Awesome Studio"));

    const quint32 microseconds = static_cast<quint32>(std::clamp(
        std::llround(60000000.0 / project.tempoBpm), 1LL, 0xffffffLL));
    appendVariableLength(track, 0);
    appendByte(track, 0xff);
    appendByte(track, 0x51);
    appendByte(track, 3);
    appendByte(track, static_cast<quint8>((microseconds >> 16) & 0xff));
    appendByte(track, static_cast<quint8>((microseconds >> 8) & 0xff));
    appendByte(track, static_cast<quint8>(microseconds & 0xff));

    int denominatorExponent = 0;
    for (int denominator = project.beatUnit; denominator > 1; denominator >>= 1)
        ++denominatorExponent;
    appendVariableLength(track, 0);
    appendByte(track, 0xff);
    appendByte(track, 0x58);
    appendByte(track, 4);
    appendByte(track, static_cast<quint8>(project.beatsPerBar));
    appendByte(track, static_cast<quint8>(denominatorExponent));
    appendByte(track, 24);
    appendByte(track, 8);

    qint64 previousTempoTick = 0;
    for (const TempoChange& change : project.tempoChanges)
    {
        const qint64 tempoTick = std::max<qint64>(previousTempoTick,
            std::llround(change.beat * exportPpqn));
        const quint32 tempoMicroseconds = static_cast<quint32>(std::clamp(
            std::llround(60000000.0 / change.bpm), 1LL, 0xffffffLL));
        appendVariableLength(track, static_cast<quint32>(tempoTick - previousTempoTick));
        appendByte(track, 0xff);
        appendByte(track, 0x51);
        appendByte(track, 3);
        appendByte(track, static_cast<quint8>((tempoMicroseconds >> 16) & 0xff));
        appendByte(track, static_cast<quint8>((tempoMicroseconds >> 8) & 0xff));
        appendByte(track, static_cast<quint8>(tempoMicroseconds & 0xff));
        previousTempoTick = tempoTick;
    }

    appendVariableLength(track, static_cast<quint32>(std::max<qint64>(0, songEndTick - previousTempoTick)));
    appendMeta(track, 0x2f, {});
    return track;
}

QByteArray makeInstrumentTrack(const InstrumentTrack& instrument, int channel,
                               double projectPitchBendRangeSemitones, qint64 songEndTick)
{
    QByteArray track;
    QByteArray name = instrument.name.toUtf8();
    if (name.size() > 255)
        name.truncate(255);
    appendVariableLength(track, 0);
    appendMeta(track, 0x03, name);

    QVector<ExportEvent> events;
    std::array<bool, 16> channelUsed{};
    for (const MidiClip& clip : instrument.clips)
    {
        for (const MidiNote& note : clip.notes)
        {
            const qint64 start = std::max<qint64>(0, std::llround((clip.startBeat + note.startBeat) * exportPpqn));
            const qint64 end = std::max(start + 1, std::llround(
                (clip.startBeat + note.startBeat + note.lengthBeats) * exportPpqn));
            const int noteChannel = note.channel >= 0 ? note.channel : channel;
            channelUsed[static_cast<size_t>(noteChannel)] = true;
            events.push_back({start, 8, std::clamp(note.pitch, 0, 127),
                              std::clamp(note.velocity, 1, 127), true, noteChannel});
            events.push_back({end, 6, std::clamp(note.pitch, 0, 127), 0, false, noteChannel});
        }
        for (const MidiPitchBend& bend : clip.pitchBends)
        {
            const qint64 tick = std::max<qint64>(0, std::llround(
                (clip.startBeat + bend.beat) * exportPpqn));
            const int value = std::clamp(bend.value, 0, 16383);
            const int bendChannel = std::clamp(bend.channel, 0, 15);
            channelUsed[static_cast<size_t>(bendChannel)] = true;
            events.push_back({tick, 7, value & 0x7f, (value >> 7) & 0x7f,
                              false, bendChannel, true});
        }
    }
    if (std::none_of(channelUsed.cbegin(), channelUsed.cend(), [](bool used) { return used; }))
        channelUsed[static_cast<size_t>(std::clamp(channel, 0, 15))] = true;

    const int pitchBendRangeHundredths = static_cast<int>(std::llround(
        projectPitchBendRangeSemitones * 100.0));
    const int rangeSemitones = pitchBendRangeHundredths / 100;
    const int rangeCents = pitchBendRangeHundredths % 100;
    for (int bendChannel = 0; bendChannel < 16; ++bendChannel)
    {
        if (!channelUsed[static_cast<size_t>(bendChannel)])
            continue;
        events.push_back({0, 0, 101, 0, false, bendChannel, false, true});
        events.push_back({0, 1, 100, 0, false, bendChannel, false, true});
        events.push_back({0, 2, 6, rangeSemitones, false, bendChannel, false, true});
        events.push_back({0, 3, 38, rangeCents, false, bendChannel, false, true});
        events.push_back({0, 4, 101, 127, false, bendChannel, false, true});
        events.push_back({0, 5, 100, 127, false, bendChannel, false, true});
    }
    std::stable_sort(events.begin(), events.end(), [](const ExportEvent& left, const ExportEvent& right)
    {
        if (left.tick != right.tick) return left.tick < right.tick;
        if (left.priority != right.priority) return left.priority < right.priority;
        if (left.channel != right.channel) return left.channel < right.channel;
        if (left.pitchBend && right.pitchBend) return false;
        return left.pitch < right.pitch;
    });

    qint64 previousTick = 0;
    for (const ExportEvent& event : events)
    {
        const qint64 tick = std::max(previousTick, event.tick);
        appendVariableLength(track, static_cast<quint32>(tick - previousTick));
        const quint8 eventStatus = event.controlChange ? 0xb0
            : event.pitchBend ? 0xe0 : (event.noteOn ? 0x90 : 0x80);
        appendByte(track, static_cast<quint8>(eventStatus | (event.channel & 0x0f)));
        appendByte(track, static_cast<quint8>(event.pitch));
        appendByte(track, static_cast<quint8>(event.velocity));
        previousTick = tick;
    }

    const qint64 finalTick = std::max(previousTick, songEndTick);
    appendVariableLength(track, static_cast<quint32>(finalTick - previousTick));
    appendMeta(track, 0x2f, {});
    return track;
}
}

bool MidiFile::load(const QString& path, ProjectData& project, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        setError(error, file.errorString());
        return false;
    }
    if (file.size() < 14 || file.size() > maximumFileBytes)
    {
        setError(error, QStringLiteral("MIDI files must be between 14 bytes and 64 MiB."));
        return false;
    }

    const QByteArray bytes = file.readAll();
    if (!validChunk(bytes, 0, "MThd"))
    {
        setError(error, QStringLiteral("The file does not have a Standard MIDI File header."));
        return false;
    }
    const quint32 headerLength = readBigEndian32(bytes, 4);
    if (headerLength < 6 || headerLength > 1024 || 8 + headerLength > static_cast<quint32>(bytes.size()))
    {
        setError(error, QStringLiteral("The MIDI file has an invalid header length."));
        return false;
    }
    const quint16 format = readBigEndian16(bytes, 8);
    const quint16 trackCount = readBigEndian16(bytes, 10);
    const quint16 division = readBigEndian16(bytes, 12);
    if (format > 1 || trackCount == 0 || trackCount > 256 ||
        (format == 0 && trackCount != 1) || (division & 0x8000) != 0 || division == 0)
    {
        setError(error, QStringLiteral("Only format 0 or 1 MIDI files with PPQN timing are supported."));
        return false;
    }

    const int ppqn = division;
    int offset = static_cast<int>(8 + headerLength);
    QVector<ParsedTrack> parsedTracks;
    parsedTracks.reserve(trackCount);
    double tempo = 120.0;
    int beatsPerBar = 4;
    int beatUnit = 4;
    bool foundTempo = false;
    bool foundTimeSignature = false;
    QVector<TempoChange> tempoChanges;
    tempoChanges.reserve(32);
    for (quint16 index = 0; index < trackCount; ++index)
    {
        if (!validChunk(bytes, offset, "MTrk"))
        {
            setError(error, QStringLiteral("The MIDI file is missing a track chunk."));
            return false;
        }
        const quint32 length = readBigEndian32(bytes, offset + 4);
        const quint64 trackEnd = static_cast<quint64>(offset) + 8 + length;
        if (trackEnd > static_cast<quint64>(bytes.size()))
        {
            setError(error, QStringLiteral("A MIDI track extends past the end of the file."));
            return false;
        }
        ParsedTrack track;
        if (!parseTrack(bytes, offset + 8, static_cast<int>(length), ppqn, track,
                        tempo, beatsPerBar, beatUnit, foundTempo, foundTimeSignature,
                        tempoChanges, error))
            return false;
        parsedTracks.push_back(std::move(track));
        offset = static_cast<int>(trackEnd);
    }

    ProjectData loaded;
    loaded.tracks.clear();
    loaded.title = QFileInfo(path).completeBaseName();
    loaded.tempoBpm = tempo;
    loaded.tempoChanges = tempoChanges;
    loaded.beatsPerBar = beatsPerBar;
    loaded.beatUnit = beatUnit;
    loaded.loopEnabled = false;
    for (const ParsedTrack& sourceTrack : parsedTracks)
    {
        if (sourceTrack.pitchBendRangeSemitones >= 0.01 &&
            sourceTrack.pitchBendRangeSemitones <= 48.0)
        {
            loaded.pitchBendRangeSemitones = sourceTrack.pitchBendRangeSemitones;
            break;
        }
    }
    double longestBeat = 0.0;
    for (const ParsedTrack& sourceTrack : parsedTracks)
    {
        if (sourceTrack.notes.isEmpty() && sourceTrack.pitchBends.isEmpty())
            continue;
        InstrumentTrack instrument;
        instrument.id = loaded.nextTrackId();
        instrument.name = sourceTrack.name.isEmpty()
            ? QStringLiteral("MIDI %1").arg(instrument.id) : sourceTrack.name;
        instrument.instrumentType = sourceTrack.percussion ? InstrumentType::DrumRack : InstrumentType::Synth;
        instrument.gain = 0.8f;
        MidiClip clip;
        clip.id = loaded.nextClipId();
        clip.name = QStringLiteral("Imported pattern");
        for (const RawNote& sourceNote : sourceTrack.notes)
        {
            const double startBeat = static_cast<double>(sourceNote.startTick) / ppqn;
            const double endBeat = static_cast<double>(sourceNote.endTick) / ppqn;
            if (endBeat > maximumSongBeats)
            {
                setError(error, QStringLiteral("This MIDI arrangement exceeds the 16,384 beat limit."));
                return false;
            }
            MidiNote note;
            note.id = loaded.nextNoteId();
            note.pitch = sourceNote.pitch;
            note.velocity = sourceNote.velocity;
            note.channel = sourceNote.channel;
            note.startBeat = startBeat;
            note.lengthBeats = std::max(1.0 / 64.0, endBeat - startBeat);
            clip.notes.push_back(note);
            longestBeat = std::max(longestBeat, endBeat);
        }
        for (const RawPitchBend& sourceBend : sourceTrack.pitchBends)
        {
            const double bendBeat = static_cast<double>(sourceBend.tick) / ppqn;
            if (bendBeat > maximumSongBeats)
            {
                setError(error, QStringLiteral("This MIDI arrangement exceeds the 16,384 beat limit."));
                return false;
            }
            clip.pitchBends.push_back({sourceBend.channel, sourceBend.value, bendBeat});
            longestBeat = std::max(longestBeat, bendBeat);
        }
        const double barLength = loaded.quarterNotesPerBar();
        clip.lengthBeats = std::clamp(std::ceil(longestBeat / barLength) * barLength,
                                      std::max(0.25, barLength), maximumSongBeats);
        instrument.clips.push_back(std::move(clip));
        loaded.tracks.push_back(std::move(instrument));
    }
    if (loaded.tracks.isEmpty())
    {
        setError(error, QStringLiteral("The MIDI file contains no playable note or pitch-bend events."));
        return false;
    }
    const double barLength = loaded.quarterNotesPerBar();
    loaded.bars = std::clamp(static_cast<int>(std::ceil(longestBeat / barLength)), 1, 64);
    loaded.normalize();
    project = std::move(loaded);
    return true;
}

bool MidiFile::save(const QString& path, const ProjectData& project, QString* error)
{
    ProjectData safeProject = project;
    safeProject.normalize();
    const double tempoMapEndBeat = safeProject.tempoChanges.isEmpty() ? 0.0 :
        safeProject.tempoChanges.back().beat;
    const qint64 songEndTick = std::llround(
        std::max(safeProject.songLengthBeats(), tempoMapEndBeat) * exportPpqn);

    QByteArray file;
    file.append("MThd", 4);
    appendBigEndian32(file, 6);
    appendBigEndian16(file, 1);
    int midiTrackCount = 1;
    for (const InstrumentTrack& track : safeProject.tracks)
        if (track.instrumentType != InstrumentType::Audio) ++midiTrackCount;
    appendBigEndian16(file, static_cast<quint16>(midiTrackCount));
    appendBigEndian16(file, exportPpqn);
    appendTrackChunk(file, makeConductorTrack(safeProject, songEndTick));
    int synthChannel = 0;
    for (const InstrumentTrack& track : safeProject.tracks)
    {
        if (track.instrumentType == InstrumentType::Audio)
            continue;
        int channel = 9;
        if (track.instrumentType != InstrumentType::DrumRack)
        {
            const int channelSlot = synthChannel++ % 15;
            channel = channelSlot < 9 ? channelSlot : channelSlot + 1;
        }
        appendTrackChunk(file, makeInstrumentTrack(track, channel,
            safeProject.pitchBendRangeSemitones, songEndTick));
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly))
    {
        setError(error, output.errorString());
        return false;
    }
    if (output.write(file) != file.size())
    {
        setError(error, output.errorString());
        return false;
    }
    if (!output.commit())
    {
        setError(error, output.errorString());
        return false;
    }
    return true;
}
