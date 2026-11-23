#include "WavExporter.h"

#include "SynthRenderer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>

namespace
{
constexpr quint16 channelCount = 2;

struct SampleFormatInfo
{
    quint16 formatTag = 1;
    quint16 bitsPerSample = 16;
    quint16 bytesPerSample = 2;
};

bool sampleFormatInfo(WavSampleFormat format, SampleFormatInfo& info)
{
    switch (format)
    {
    case WavSampleFormat::Pcm16:
        info = {1, 16, 2};
        return true;
    case WavSampleFormat::Pcm24:
        info = {1, 24, 3};
        return true;
    case WavSampleFormat::Float32:
        info = {3, 32, 4};
        return true;
    }
    return false;
}

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

void writeIntegerSample(char* destination, qint64 value, int byteCount)
{
    const quint64 bits = static_cast<quint64>(value);
    for (int byte = 0; byte < byteCount; ++byte)
        destination[byte] = static_cast<char>((bits >> (byte * 8)) & 0xff);
}

void writeFloatSample(char* destination, float value)
{
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "WAV float samples require 32-bit floats");
    std::memcpy(&bits, &value, sizeof(bits));
    write32(destination, bits);
}

float outputSample(float sample, float masterGain, bool softClip)
{
    const float gained = sample * masterGain;
    if (!std::isfinite(gained))
        return 0.0f;
    return softClip ? std::clamp(std::tanh(gained), -1.0f, 1.0f) : gained;
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

bool WavExporter::exportProject(const ProjectData& project, const QString& path, QString* error,
                                double startBeat, double endBeat, const WavExportOptions& options)
{
    ProjectData safeProject = project;
    safeProject.normalize();
    const bool customRange = endBeat >= 0.0;
    if (customRange && (!std::isfinite(startBeat) || !std::isfinite(endBeat) || startBeat < 0.0 ||
                        endBeat <= startBeat || endBeat > safeProject.songLengthBeats()))
    {
        setError(error, QStringLiteral("Choose a beat range inside the arrangement with an end after its start."));
        return false;
    }
    if (customRange)
        safeProject.countInBars = 0;

    switch (options.sampleRate)
    {
    case 44100:
    case 48000:
    case 88200:
    case 96000:
    case 192000:
        break;
    default:
        setError(error, QStringLiteral("Choose a supported WAV sample rate."));
        return false;
    }
    const quint32 sampleRate = static_cast<quint32>(options.sampleRate);

    SampleFormatInfo formatInfo;
    if (!sampleFormatInfo(options.sampleFormat, formatInfo))
    {
        setError(error, QStringLiteral("Choose a supported WAV sample format."));
        return false;
    }
    const bool integerPcm = formatInfo.formatTag == 1;
    if (options.dither && !integerPcm)
    {
        setError(error, QStringLiteral("Dither is available for integer PCM output only."));
        return false;
    }
    const quint32 bytesPerSampleFrame = channelCount * formatInfo.bytesPerSample;

    auto snapshot = std::make_shared<const ProjectData>(safeProject);
    SynthRenderer renderer;
    renderer.setProject(snapshot, sampleRate);

    qint64 countInFrames = 0;
    qint64 rangeStartFrame = 0;
    qint64 frameCount = 0;
    if (customRange)
    {
        const double framesPerBeat = sampleRate * 60.0 / safeProject.tempoBpm;
        rangeStartFrame = static_cast<qint64>(std::llround(startBeat * framesPerBeat));
        const qint64 rangeEndFrame = static_cast<qint64>(std::llround(endBeat * framesPerBeat));
        frameCount = rangeEndFrame - rangeStartFrame;
        if (frameCount <= 0)
        {
            setError(error, QStringLiteral("The selected beat range is shorter than one audio frame."));
            return false;
        }
    }
    else
    {
        const qint64 beatUnitFrames = static_cast<qint64>(std::llround(
            sampleRate * 60.0 / safeProject.tempoBpm * 4.0 / safeProject.beatUnit));
        countInFrames = static_cast<qint64>(safeProject.countInBars) *
                        safeProject.beatsPerBar * beatUnitFrames;
        const double durationSeconds = safeProject.songLengthBeats() * 60.0 / safeProject.tempoBpm +
                                       renderer.effectTailSeconds() + 0.1;
        frameCount = countInFrames + static_cast<qint64>(std::ceil(durationSeconds * sampleRate));
    }
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
    write16(header + 20, formatInfo.formatTag);
    write16(header + 22, channelCount);
    write32(header + 24, sampleRate);
    write32(header + 28, sampleRate * bytesPerSampleFrame);
    write16(header + 32, bytesPerSampleFrame);
    write16(header + 34, formatInfo.bitsPerSample);
    std::memcpy(header + 36, "data", 4);
    write32(header + 40, static_cast<quint32>(dataSize));
    if (file.write(header, sizeof(header)) != sizeof(header))
    {
        setError(error, file.errorString());
        return false;
    }

    constexpr int framesPerChunk = 2048;
    QByteArray audio(framesPerChunk * bytesPerSampleFrame, Qt::Uninitialized);
    std::mt19937 ditherGenerator(0x41575331u);
    std::uniform_real_distribution<double> unitNoise(0.0, 1.0);
    qint64 writtenFrames = 0;
    while (writtenFrames < frameCount)
    {
        const int frames = static_cast<int>(std::min<qint64>(framesPerChunk, frameCount - writtenFrames));
        for (int index = 0; index < frames; ++index)
        {
            renderer.beginMixerFrame();
            const qint64 projectFrame = customRange
                ? rangeStartFrame + writtenFrames + index
                : writtenFrames + index - countInFrames;
            StereoSample rendered = renderer.renderSample(projectFrame);
            const StereoSample mixerOutput = renderer.finishMixerFrame();
            rendered.left += mixerOutput.left;
            rendered.right += mixerOutput.right;
            rendered = renderer.processMasterEffects(rendered);
            char* frame = audio.data() + index * bytesPerSampleFrame;
            const float output[2] = {
                outputSample(rendered.left, safeProject.masterGain,
                             options.sampleFormat != WavSampleFormat::Float32),
                outputSample(rendered.right, safeProject.masterGain,
                             options.sampleFormat != WavSampleFormat::Float32)
            };
            for (int channel = 0; channel < 2; ++channel)
            {
                char* destination = frame + channel * formatInfo.bytesPerSample;
                if (options.sampleFormat == WavSampleFormat::Float32)
                {
                    writeFloatSample(destination, output[channel]);
                    continue;
                }

                const int shift = formatInfo.bitsPerSample - 1;
                const qint64 maximum = (qint64{1} << shift) - 1;
                const qint64 minimum = -(qint64{1} << shift);
                double scaled = static_cast<double>(output[channel]) * maximum;
                if (options.dither)
                {
                    const double firstNoise = unitNoise(ditherGenerator);
                    const double secondNoise = unitNoise(ditherGenerator);
                    scaled += firstNoise - secondNoise;
                }
                const qint64 quantized = static_cast<qint64>(std::lrint(scaled));
                writeIntegerSample(destination, std::clamp(quantized, minimum, maximum),
                                   formatInfo.bytesPerSample);
            }
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

bool WavExporter::exportStems(const ProjectData& project, const QString& directoryPath, QString* error,
                              double startBeat, double endBeat, const WavExportOptions& options)
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
        if (!exportProject(stemProject, paths[trackIndex], &renderError, startBeat, endBeat, options))
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
