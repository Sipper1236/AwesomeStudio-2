#include "WavExporter.h"

#include "SynthRenderer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace
{
constexpr quint32 sampleRate = 44100;
constexpr quint16 channelCount = 2;
constexpr quint16 bitsPerSample = 16;
constexpr quint32 bytesPerSampleFrame = channelCount * bitsPerSample / 8;

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

void write16(char* destination, quint16 value)
{
    destination[0] = static_cast<char>(value & 0xff);
    destination[1] = static_cast<char>((value >> 8) & 0xff);
}

void write32(char* destination, quint32 value)
{
    destination[0] = static_cast<char>(value & 0xff);
    destination[1] = static_cast<char>((value >> 8) & 0xff);
    destination[2] = static_cast<char>((value >> 16) & 0xff);
    destination[3] = static_cast<char>((value >> 24) & 0xff);
}

QString safeStemName(const QString& source)
{
    QString result;
    for (const QChar character : source.trimmed())
    {
        if (character.isLetterOrNumber() || character == QLatin1Char('-') || character == QLatin1Char('_'))
        {
            result.append(character);
        }
        else if (!result.isEmpty() && !result.endsWith(QLatin1Char('_')))
        {
            result.append(QLatin1Char('_'));
        }
    }
    while (result.endsWith(QLatin1Char('_')))
        result.chop(1);
    return result.isEmpty() ? QStringLiteral("Track") : result;
}
}

bool WavExporter::exportProject(const ProjectData& project, const QString& path, QString* error)
{
    ProjectData safeProject = project;
    safeProject.normalize();

    auto snapshot = std::make_shared<const ProjectData>(safeProject);
    SynthRenderer renderer;
    renderer.setProject(snapshot, sampleRate);

    const qint64 beatUnitFrames = static_cast<qint64>(std::llround(
        sampleRate * 60.0 / safeProject.tempoBpm * 4.0 / safeProject.beatUnit));
    const qint64 countInFrames = static_cast<qint64>(safeProject.countInBars) *
                                 safeProject.beatsPerBar * beatUnitFrames;
    const double durationSeconds = safeProject.songLengthBeats() * 60.0 / safeProject.tempoBpm +
                                   renderer.effectTailSeconds() + 0.1;
    const qint64 frameCount = countInFrames + static_cast<qint64>(std::ceil(durationSeconds * sampleRate));
    const quint64 dataSize = static_cast<quint64>(frameCount) * bytesPerSampleFrame;
    if (dataSize > std::numeric_limits<quint32>::max() - 36)
    {
        setError(error, QStringLiteral("This pattern is too long to store in a standard WAV file."));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        setError(error, file.errorString());
        return false;
    }

    char header[44]{};
    std::memcpy(header, "RIFF", 4);
    write32(header + 4, static_cast<quint32>(dataSize + 36));
    std::memcpy(header + 8, "WAVEfmt ", 8);
    write32(header + 16, 16);
    write16(header + 20, 1);
    write16(header + 22, channelCount);
    write32(header + 24, sampleRate);
    write32(header + 28, sampleRate * bytesPerSampleFrame);
    write16(header + 32, bytesPerSampleFrame);
    write16(header + 34, bitsPerSample);
    std::memcpy(header + 36, "data", 4);
    write32(header + 40, static_cast<quint32>(dataSize));
    if (file.write(header, sizeof(header)) != sizeof(header))
    {
        setError(error, file.errorString());
        return false;
    }

    constexpr int framesPerChunk = 2048;
    QByteArray audio(framesPerChunk * bytesPerSampleFrame, Qt::Uninitialized);
    qint64 writtenFrames = 0;
    while (writtenFrames < frameCount)
    {
        const int frames = static_cast<int>(std::min<qint64>(framesPerChunk, frameCount - writtenFrames));
        for (int index = 0; index < frames; ++index)
        {
            renderer.beginMixerFrame();
            StereoSample rendered = renderer.renderSample(writtenFrames + index - countInFrames);
            const StereoSample mixerOutput = renderer.finishMixerFrame();
            rendered.left += mixerOutput.left;
            rendered.right += mixerOutput.right;
            rendered = renderer.processMasterEffects(rendered);
            const qint16 left = static_cast<qint16>(std::lrint(std::clamp(
                std::tanh(rendered.left * safeProject.masterGain), -1.0f, 1.0f) * 32767.0f));
            const qint16 right = static_cast<qint16>(std::lrint(std::clamp(
                std::tanh(rendered.right * safeProject.masterGain), -1.0f, 1.0f) * 32767.0f));
            char* frame = audio.data() + index * bytesPerSampleFrame;
            write16(frame, static_cast<quint16>(left));
            write16(frame + 2, static_cast<quint16>(right));
        }

        const qint64 bytes = static_cast<qint64>(frames) * bytesPerSampleFrame;
        if (file.write(audio.constData(), bytes) != bytes)
        {
            setError(error, file.errorString());
            return false;
        }
        writtenFrames += frames;
    }

    if (!file.commit())
    {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool WavExporter::exportStems(const ProjectData& project, const QString& directoryPath, QString* error)
{
    ProjectData source = project;
    source.normalize();
    if (source.tracks.isEmpty())
    {
        setError(error, QStringLiteral("The project has no tracks to export."));
        return false;
    }

    const QDir directory(directoryPath);
    if (!directory.exists())
    {
        setError(error, QStringLiteral("The selected stem output folder does not exist."));
        return false;
    }

    QVector<QString> paths;
    paths.reserve(source.tracks.size());
    for (int index = 0; index < source.tracks.size(); ++index)
    {
        const InstrumentTrack& track = source.tracks[index];
        const QString fileName = QStringLiteral("%1_%2_%3.wav")
            .arg(index + 1, 3, 10, QLatin1Char('0'))
            .arg(track.id)
            .arg(safeStemName(track.name));
        const QString path = directory.filePath(fileName);
        if (QFileInfo::exists(path))
        {
            setError(error, QStringLiteral("Stem file already exists: %1. Choose an empty folder.").arg(fileName));
            return false;
        }
        paths.push_back(path);
    }

    QVector<QString> createdPaths;
    createdPaths.reserve(paths.size());
    for (int trackIndex = 0; trackIndex < source.tracks.size(); ++trackIndex)
    {
        ProjectData stemProject = source;
        for (int index = 0; index < stemProject.tracks.size(); ++index)
        {
            stemProject.tracks[index].muted = index != trackIndex;
            stemProject.tracks[index].solo = false;
        }
        for (MixerBus& bus : stemProject.buses)
        {
            bus.muted = false;
            bus.solo = false;
        }

        QString renderError;
        if (!exportProject(stemProject, paths[trackIndex], &renderError))
        {
            for (const QString& createdPath : createdPaths)
                QFile::remove(createdPath);
            setError(error, QStringLiteral("Could not export %1: %2")
                                  .arg(QFileInfo(paths[trackIndex]).fileName(), renderError));
            return false;
        }
        createdPaths.push_back(paths[trackIndex]);
    }
    return true;
}
