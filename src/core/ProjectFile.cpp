#include "ProjectFile.h"

#include "AudioFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <cmath>
#include <limits>

namespace
{
constexpr int currentProjectVersion = 13;
constexpr auto projectFormat = "awesome-studio-project";

struct MediaLoadContext
{
    QHash<QString, std::shared_ptr<const AudioAsset>> assets;
    QSet<QString> unavailableAssets;
    quint64 decodedBytes = 0;
};

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

void loadAudioReference(QString& path, const QString& mediaBaseDir, MediaLoadContext& media,
                        std::shared_ptr<const AudioAsset>& asset)
{
    if (path.isEmpty())
        return;
    if (QDir::isRelativePath(path))
        path = QDir(mediaBaseDir).absoluteFilePath(path);

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || !fileInfo.isFile())
        return;
    QString cacheKey = fileInfo.canonicalFilePath();
    if (cacheKey.isEmpty())
        cacheKey = fileInfo.absoluteFilePath();
    const auto cached = media.assets.constFind(cacheKey);
    if (cached != media.assets.cend())
    {
        asset = *cached;
        return;
    }
    if (media.unavailableAssets.contains(cacheKey))
        return;

    std::shared_ptr<const AudioAsset> decoded;
    QString audioError;
    if (!AudioFile::loadWav(path, decoded, &audioError))
    {
        media.unavailableAssets.insert(cacheKey);
        return;
    }
    const quint64 bytes = static_cast<quint64>(decoded->samples.size() + decoded->waveformPeaks.size()) *
                          sizeof(float);
    if (bytes > AudioFile::maximumProjectDecodedBytes - media.decodedBytes)
    {
        media.unavailableAssets.insert(cacheKey);
        return;
    }
    media.decodedBytes += bytes;
    media.assets.insert(cacheKey, decoded);
    asset = std::move(decoded);
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
            {QStringLiteral("lengthBeats"), note.lengthBeats},
            {QStringLiteral("channel"), note.channel}
        });
    }
    return result;
}

QJsonArray saveAutomation(const QVector<AutomationPoint>& points)
{
    QJsonArray result;
    for (const AutomationPoint& point : points)
    {
        result.push_back(QJsonObject{
            {QStringLiteral("beat"), point.beat},
            {QStringLiteral("value"), point.value}
        });
    }
    return result;
}

bool loadAutomation(const QJsonValue& value, QVector<AutomationPoint>& points,
                    const QString& parameterName, QString* error)
{
    if (value.isUndefined())
        return true;
    if (!value.isArray() || value.toArray().size() > 10000)
    {
        setError(error, QStringLiteral("The track has invalid or excessive %1 automation data.")
                           .arg(parameterName));
        return false;
    }
    for (const QJsonValue& item : value.toArray())
    {
        if (!item.isObject())
        {
            setError(error, QStringLiteral("The track contains an invalid %1 automation point.")
                               .arg(parameterName));
            return false;
        }
        const QJsonObject object = item.toObject();
        AutomationPoint point;
        point.beat = object.value(QStringLiteral("beat")).toDouble(-1.0);
        point.value = static_cast<float>(object.value(QStringLiteral("value")).toDouble(
            std::numeric_limits<double>::quiet_NaN()));
        if (!std::isfinite(point.beat) || !std::isfinite(point.value) || point.beat < 0.0)
        {
            setError(error, QStringLiteral("The track contains an out-of-range %1 automation point.")
                               .arg(parameterName));
            return false;
        }
        points.push_back(point);
    }
    return true;
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
        note.channel = object.value(QStringLiteral("channel")).toInt(-1);
        if (note.pitch < 0 || note.pitch > 127 || note.velocity < 1 || note.velocity > 127 ||
            note.startBeat < 0.0 || note.lengthBeats <= 0.0 || note.channel < -1 || note.channel > 15)
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
        {QStringLiteral("notes"), saveNotes(clip.notes)},
        {QStringLiteral("audioSourcePath"), clip.audioSourcePath},
        {QStringLiteral("audioStartFrame"), static_cast<qint64>(clip.audioStartFrame)},
        {QStringLiteral("audioFadeInBeats"), clip.audioFadeInBeats},
        {QStringLiteral("audioFadeOutBeats"), clip.audioFadeOutBeats}
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
        {QStringLiteral("instrumentType"), static_cast<int>(track.instrumentType)},
        {QStringLiteral("waveform"), static_cast<int>(track.waveform)},
        {QStringLiteral("attackSeconds"), track.attackSeconds},
        {QStringLiteral("decaySeconds"), track.decaySeconds},
        {QStringLiteral("sustainLevel"), track.sustainLevel},
        {QStringLiteral("releaseSeconds"), track.releaseSeconds},
        {QStringLiteral("gain"), track.gain},
        {QStringLiteral("pan"), track.pan},
        {QStringLiteral("eqLowDb"), track.eqLowDb},
        {QStringLiteral("eqMidDb"), track.eqMidDb},
        {QStringLiteral("eqHighDb"), track.eqHighDb},
        {QStringLiteral("volumeAutomation"), saveAutomation(track.volumeAutomation)},
        {QStringLiteral("panAutomation"), saveAutomation(track.panAutomation)},
        {QStringLiteral("muted"), track.muted},
        {QStringLiteral("solo"), track.solo},
        {QStringLiteral("drumSamples"), [&track]
         {
             QJsonArray samples;
             for (const DrumPadSample& sample : track.drumSamples)
                 samples.push_back(sample.path);
             return samples;
         }()},
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

bool loadClip(const QJsonValue& value, MidiClip& clip, const QString& mediaBaseDir,
              MediaLoadContext& media, QString* error)
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
    clip.audioSourcePath = object.value(QStringLiteral("audioSourcePath")).toString();
    const qint64 storedAudioStart = object.value(QStringLiteral("audioStartFrame")).toInteger(0);
    if (storedAudioStart < 0)
    {
        setError(error, QStringLiteral("The project contains an invalid audio trim position."));
        return false;
    }
    clip.audioStartFrame = storedAudioStart;
    clip.audioFadeInBeats = object.value(QStringLiteral("audioFadeInBeats")).toDouble(0.0);
    clip.audioFadeOutBeats = object.value(QStringLiteral("audioFadeOutBeats")).toDouble(0.0);
    loadAudioReference(clip.audioSourcePath, mediaBaseDir, media, clip.audioSource);
    return loadNotes(object.value(QStringLiteral("notes")), clip.notes, error);
}

bool loadTrack(const QJsonValue& value, InstrumentTrack& track, const QString& mediaBaseDir,
               MediaLoadContext& media, int version, QString* error)
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
    track.instrumentType = static_cast<InstrumentType>(object.value(QStringLiteral("instrumentType")).toInt(0));
    track.waveform = static_cast<SynthWaveform>(object.value(QStringLiteral("waveform")).toInt(0));
    track.attackSeconds = static_cast<float>(
        object.value(QStringLiteral("attackSeconds")).toDouble(track.attackSeconds));
    track.decaySeconds = static_cast<float>(
        object.value(QStringLiteral("decaySeconds")).toDouble(track.decaySeconds));
    track.sustainLevel = static_cast<float>(
        object.value(QStringLiteral("sustainLevel")).toDouble(track.sustainLevel));
    track.releaseSeconds = static_cast<float>(
        object.value(QStringLiteral("releaseSeconds")).toDouble(track.releaseSeconds));
    track.gain = static_cast<float>(object.value(QStringLiteral("gain")).toDouble(0.8));
    track.pan = static_cast<float>(object.value(QStringLiteral("pan")).toDouble(0.0));
    if (version >= 13)
    {
        track.eqLowDb = static_cast<float>(object.value(QStringLiteral("eqLowDb")).toDouble(0.0));
        track.eqMidDb = static_cast<float>(object.value(QStringLiteral("eqMidDb")).toDouble(0.0));
        track.eqHighDb = static_cast<float>(object.value(QStringLiteral("eqHighDb")).toDouble(0.0));
    }
    if (!loadAutomation(object.value(QStringLiteral("volumeAutomation")), track.volumeAutomation,
                        QStringLiteral("volume"), error) ||
        !loadAutomation(object.value(QStringLiteral("panAutomation")), track.panAutomation,
                        QStringLiteral("pan"), error))
        return false;
    track.muted = object.value(QStringLiteral("muted")).toBool(false);
    track.solo = object.value(QStringLiteral("solo")).toBool(false);
    const QJsonValue drumSampleValue = object.value(QStringLiteral("drumSamples"));
    if (drumSampleValue.isArray())
    {
        const QJsonArray drumSamples = drumSampleValue.toArray();
        if (drumSamples.size() > static_cast<qsizetype>(track.drumSamples.size()))
        {
            setError(error, QStringLiteral("The drum rack contains too many sample assignments."));
            return false;
        }
        for (qsizetype index = 0; index < drumSamples.size(); ++index)
        {
            if (!drumSamples[index].isString())
            {
                setError(error, QStringLiteral("The drum rack contains an invalid sample path."));
                return false;
            }
            track.drumSamples[static_cast<size_t>(index)].path = drumSamples[index].toString();
            loadAudioReference(track.drumSamples[static_cast<size_t>(index)].path,
                               mediaBaseDir, media, track.drumSamples[static_cast<size_t>(index)].source);
        }
    }
    else if (!drumSampleValue.isUndefined() && !drumSampleValue.isNull())
    {
        setError(error, QStringLiteral("The drum rack has invalid sample assignments."));
        return false;
    }
    const QJsonValue clipValue = object.value(QStringLiteral("clips"));
    if (!clipValue.isArray() || clipValue.toArray().size() > 10000)
    {
        setError(error, QStringLiteral("The track has invalid or excessive clip data."));
        return false;
    }
    for (const QJsonValue& clipJson : clipValue.toArray())
    {
        MidiClip clip;
        if (!loadClip(clipJson, clip, mediaBaseDir, media, error))
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
    QJsonArray markers;
    for (const TimelineMarker& marker : safeProject.markers)
        markers.push_back(QJsonObject{
            {QStringLiteral("beat"), marker.beat},
            {QStringLiteral("name"), marker.name}
        });

    const QJsonObject root{
        {QStringLiteral("format"), QString::fromLatin1(projectFormat)},
        {QStringLiteral("version"), currentProjectVersion},
        {QStringLiteral("title"), safeProject.title},
        {QStringLiteral("tempoBpm"), safeProject.tempoBpm},
        {QStringLiteral("beatsPerBar"), safeProject.beatsPerBar},
        {QStringLiteral("beatUnit"), safeProject.beatUnit},
        {QStringLiteral("bars"), safeProject.bars},
        {QStringLiteral("loopEnabled"), safeProject.loopEnabled},
        {QStringLiteral("metronomeEnabled"), safeProject.metronomeEnabled},
        {QStringLiteral("countInBars"), safeProject.countInBars},
        {QStringLiteral("masterGain"), safeProject.masterGain},
        {QStringLiteral("markers"), markers},
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

    const QString mediaBaseDir = QFileInfo(path).absoluteDir().absolutePath();
    MediaLoadContext media;
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
    loaded.metronomeEnabled = version >= 5 ? root.value(QStringLiteral("metronomeEnabled")).toBool(false) : false;
    loaded.countInBars = version >= 5 ? root.value(QStringLiteral("countInBars")).toInt(0) : 0;
    loaded.masterGain = static_cast<float>(root.value(QStringLiteral("masterGain")).toDouble(0.8));
    if (version >= 12)
    {
        const QJsonValue markerValue = root.value(QStringLiteral("markers"));
        if (!markerValue.isArray() || markerValue.toArray().size() > 1000)
        {
            setError(error, QStringLiteral("The project has invalid or excessive timeline markers."));
            return false;
        }
        for (const QJsonValue& markerJson : markerValue.toArray())
        {
            if (!markerJson.isObject())
            {
                setError(error, QStringLiteral("The project contains an invalid timeline marker."));
                return false;
            }
            const QJsonObject markerObject = markerJson.toObject();
            TimelineMarker marker;
            marker.beat = markerObject.value(QStringLiteral("beat")).toDouble(-1.0);
            marker.name = markerObject.value(QStringLiteral("name")).toString();
            if (!std::isfinite(marker.beat) || marker.beat < 0.0 || marker.beat > 16384.0 ||
                marker.name.trimmed().isEmpty())
            {
                setError(error, QStringLiteral("The project contains an out-of-range timeline marker."));
                return false;
            }
            loaded.markers.push_back(std::move(marker));
        }
    }

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
            if (!loadTrack(trackJson, track, mediaBaseDir, media, version, error))
                return false;
            loaded.tracks.push_back(std::move(track));
        }
    }

    loaded.normalize();
    project = std::move(loaded);
    return true;
}
