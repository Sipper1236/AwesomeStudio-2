#include "ProjectFile.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace
{
constexpr int currentProjectVersion = 3;
constexpr auto projectFormat = "awesome-studio-project";

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

QJsonArray saveNotes(const QVector<MidiNote>& notes)
{
    QJsonArray result;
    for (const MidiNote& note : notes)
    {
        result.push_back(QJsonObject{
            {QStringLiteral("id"), static_cast<qint64>(note.id)},
            {QStringLiteral("pitch"), note.pitch},
            {QStringLiteral("velocity"), note.velocity},
            {QStringLiteral("startBeat"), note.startBeat},
            {QStringLiteral("lengthBeats"), note.lengthBeats}
        });
    }
    return result;
}

bool loadNotes(const QJsonValue& value, QVector<MidiNote>& notes, QString* error)
{
    if (!value.isArray())
    {
        setError(error, QStringLiteral("The project is missing MIDI note data."));
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() > 100000)
    {
        setError(error, QStringLiteral("This project contains too many MIDI notes to open safely."));
        return false;
    }

    for (const QJsonValue& item : array)
    {
        if (!item.isObject())
        {
            setError(error, QStringLiteral("The project contains an invalid MIDI note."));
            return false;
        }

        const QJsonObject object = item.toObject();
        const qint64 storedId = object.value(QStringLiteral("id")).toInteger();
        MidiNote note;
        if (storedId < 0)
        {
            setError(error, QStringLiteral("The project contains an invalid MIDI note ID."));
            return false;
        }
        note.id = static_cast<quint64>(storedId);
        note.pitch = object.value(QStringLiteral("pitch")).toInt(-1);
        note.velocity = object.value(QStringLiteral("velocity")).toInt(-1);
        note.startBeat = object.value(QStringLiteral("startBeat")).toDouble(-1.0);
        note.lengthBeats = object.value(QStringLiteral("lengthBeats")).toDouble(-1.0);
        if (note.pitch < 0 || note.pitch > 127 || note.velocity < 1 || note.velocity > 127 ||
            note.startBeat < 0.0 || note.lengthBeats <= 0.0)
        {
            setError(error, QStringLiteral("The project contains an out-of-range MIDI note."));
            return false;
        }
        notes.push_back(note);
    }
    return true;
}

QJsonObject saveClip(const MidiClip& clip)
{
    return QJsonObject{
        {QStringLiteral("id"), static_cast<qint64>(clip.id)},
        {QStringLiteral("name"), clip.name},
        {QStringLiteral("startBeat"), clip.startBeat},
        {QStringLiteral("lengthBeats"), clip.lengthBeats},
        {QStringLiteral("notes"), saveNotes(clip.notes)}
    };
}

QJsonObject saveTrack(const InstrumentTrack& track)
{
    QJsonArray clips;
    for (const MidiClip& clip : track.clips)
        clips.push_back(saveClip(clip));

    return QJsonObject{
        {QStringLiteral("id"), static_cast<qint64>(track.id)},
        {QStringLiteral("name"), track.name},
        {QStringLiteral("waveform"), static_cast<int>(track.waveform)},
        {QStringLiteral("gain"), track.gain},
        {QStringLiteral("pan"), track.pan},
        {QStringLiteral("muted"), track.muted},
        {QStringLiteral("solo"), track.solo},
        {QStringLiteral("clips"), clips}
    };
}

bool readId(const QJsonObject& object, const QString& key, quint64& id, QString* error)
{
    const qint64 value = object.value(key).toInteger();
    if (value < 0)
    {
        setError(error, QStringLiteral("The project contains an invalid %1 ID.").arg(key));
        return false;
    }
    id = static_cast<quint64>(value);
    return true;
}

bool loadClip(const QJsonValue& value, MidiClip& clip, QString* error)
{
    if (!value.isObject())
    {
        setError(error, QStringLiteral("The project contains an invalid MIDI clip."));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!readId(object, QStringLiteral("id"), clip.id, error))
        return false;
    clip.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Pattern"));
    clip.startBeat = object.value(QStringLiteral("startBeat")).toDouble(-1.0);
    clip.lengthBeats = object.value(QStringLiteral("lengthBeats")).toDouble(-1.0);
    if (clip.startBeat < 0.0 || clip.lengthBeats <= 0.0)
    {
        setError(error, QStringLiteral("The project contains an out-of-range MIDI clip."));
        return false;
    }
    return loadNotes(object.value(QStringLiteral("notes")), clip.notes, error);
}

bool loadTrack(const QJsonValue& value, InstrumentTrack& track, QString* error)
{
    if (!value.isObject())
    {
        setError(error, QStringLiteral("The project contains an invalid instrument track."));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!readId(object, QStringLiteral("id"), track.id, error))
        return false;
    track.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Synth"));
    track.waveform = static_cast<SynthWaveform>(object.value(QStringLiteral("waveform")).toInt(0));
    track.gain = static_cast<float>(object.value(QStringLiteral("gain")).toDouble(0.8));
    track.pan = static_cast<float>(object.value(QStringLiteral("pan")).toDouble(0.0));
    track.muted = object.value(QStringLiteral("muted")).toBool(false);
    track.solo = object.value(QStringLiteral("solo")).toBool(false);
    const QJsonValue clipValue = object.value(QStringLiteral("clips"));
    if (!clipValue.isArray() || clipValue.toArray().size() > 10000)
    {
        setError(error, QStringLiteral("The track has invalid or excessive clip data."));
        return false;
    }
    for (const QJsonValue& clipJson : clipValue.toArray())
    {
        MidiClip clip;
        if (!loadClip(clipJson, clip, error))
            return false;
        track.clips.push_back(std::move(clip));
    }
    return true;
}
}

bool ProjectFile::save(const QString& path, const ProjectData& project, QString* error)
{
    ProjectData safeProject = project;
    safeProject.normalize();

    QJsonArray tracks;
    for (const InstrumentTrack& track : safeProject.tracks)
        tracks.push_back(saveTrack(track));

    const QJsonObject root{
        {QStringLiteral("format"), QString::fromLatin1(projectFormat)},
        {QStringLiteral("version"), currentProjectVersion},
        {QStringLiteral("title"), safeProject.title},
        {QStringLiteral("tempoBpm"), safeProject.tempoBpm},
        {QStringLiteral("beatsPerBar"), safeProject.beatsPerBar},
        {QStringLiteral("beatUnit"), safeProject.beatUnit},
        {QStringLiteral("bars"), safeProject.bars},
        {QStringLiteral("loopEnabled"), safeProject.loopEnabled},
        {QStringLiteral("masterGain"), safeProject.masterGain},
        {QStringLiteral("tracks"), tracks}
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        setError(error, file.errorString());
        return false;
    }

    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size())
    {
        setError(error, file.errorString());
        return false;
    }
    if (!file.commit())
    {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool ProjectFile::load(const QString& path, ProjectData& project, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        setError(error, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        setError(error, parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QString::fromLatin1(projectFormat))
    {
        setError(error, QStringLiteral("This file is not an Awesome Studio project."));
        return false;
    }
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > currentProjectVersion)
    {
        setError(error, QStringLiteral("This project uses an unsupported file version."));
        return false;
    }

    ProjectData loaded;
    loaded.tracks.clear();
    loaded.title = root.value(QStringLiteral("title")).toString(QStringLiteral("Untitled"));
    loaded.tempoBpm = root.value(QStringLiteral("tempoBpm")).toDouble(120.0);
    loaded.beatsPerBar = root.value(QStringLiteral("beatsPerBar")).toInt(4);
    loaded.beatUnit = version >= 3 ? root.value(QStringLiteral("beatUnit")).toInt(4) : 4;
    loaded.bars = root.value(QStringLiteral("bars")).toInt(4);
    loaded.loopEnabled = root.value(QStringLiteral("loopEnabled")).toBool(true);
    loaded.masterGain = static_cast<float>(root.value(QStringLiteral("masterGain")).toDouble(0.8));

    if (version == 1)
    {
        InstrumentTrack track;
        track.id = 1;
        track.name = QStringLiteral("Synth 1");
        track.waveform = static_cast<SynthWaveform>(root.value(QStringLiteral("waveform")).toInt(0));
        track.gain = 1.0f;
        MidiClip clip;
        clip.id = 1;
        clip.name = QStringLiteral("Pattern 1");
        clip.lengthBeats = loaded.patternLengthBeats();
        if (!loadNotes(root.value(QStringLiteral("notes")), clip.notes, error))
            return false;
        track.clips.push_back(std::move(clip));
        loaded.tracks.push_back(std::move(track));
    }
    else
    {
        const QJsonValue trackValue = root.value(QStringLiteral("tracks"));
        if (!trackValue.isArray() || trackValue.toArray().size() > 256)
        {
            setError(error, QStringLiteral("The project has invalid or excessive track data."));
            return false;
        }
        for (const QJsonValue& trackJson : trackValue.toArray())
        {
            InstrumentTrack track;
            if (!loadTrack(trackJson, track, error))
                return false;
            loaded.tracks.push_back(std::move(track));
        }
    }

    loaded.normalize();
    project = std::move(loaded);
    return true;
}
