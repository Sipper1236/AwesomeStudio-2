#include "AudioFile.h"

#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>
#include <algorithm>
#include <utility>

namespace
{
constexpr qint64 maximumWavBytes = 128 * 1024 * 1024;
constexpr quint64 maximumWavSamples = 40000000ULL;

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

quint8 byteAt(const QByteArray& data, int offset)
{
    return static_cast<quint8>(static_cast<unsigned char>(data.at(offset)));
}

quint16 read16(const QByteArray& data, int offset)
{
    return static_cast<quint16>(byteAt(data, offset) |
                                (static_cast<quint16>(byteAt(data, offset + 1)) << 8));
}

quint32 read32(const QByteArray& data, int offset)
{
    return static_cast<quint32>(byteAt(data, offset)) |
           (static_cast<quint32>(byteAt(data, offset + 1)) << 8) |
           (static_cast<quint32>(byteAt(data, offset + 2)) << 16) |
           (static_cast<quint32>(byteAt(data, offset + 3)) << 24);
}

float decodeSample(const QByteArray& bytes, int offset, quint16 format, quint16 bitsPerSample)
{
    if (format == 3)
    {
        const quint32 bits = read32(bytes, offset);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value) ? value : 0.0f;
    }

    if (bitsPerSample == 8)
        return (static_cast<float>(byteAt(bytes, offset)) - 128.0f) / 128.0f;
    if (bitsPerSample == 16)
    {
        const quint16 raw = read16(bytes, offset);
        qint16 value = 0;
        std::memcpy(&value, &raw, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }
    if (bitsPerSample == 24)
    {
        qint32 value = static_cast<qint32>(byteAt(bytes, offset)) |
                       (static_cast<qint32>(byteAt(bytes, offset + 1)) << 8) |
                       (static_cast<qint32>(byteAt(bytes, offset + 2)) << 16);
        if ((value & 0x800000) != 0)
            value -= 0x1000000;
        return static_cast<float>(value) / 8388608.0f;
    }
    const quint32 raw = read32(bytes, offset);
    qint32 value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    return static_cast<float>(static_cast<double>(value) / 2147483648.0);
}
}

bool AudioFile::loadWav(const QString& path, std::shared_ptr<const AudioAsset>& asset, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        setError(error, file.errorString());
        return false;
    }
    if (file.size() < 44 || file.size() > maximumWavBytes)
    {
        setError(error, QStringLiteral("WAV files must be between 44 bytes and 128 MiB."));
        return false;
    }

    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
    {
        setError(error, QStringLiteral("The WAV file could not be read completely."));
        return false;
    }
    if (bytes.mid(0, 4) != QByteArrayLiteral("RIFF") || bytes.mid(8, 4) != QByteArrayLiteral("WAVE"))
    {
        setError(error, QStringLiteral("This is not a RIFF/WAVE file."));
        return false;
    }
    const quint64 riffEnd = static_cast<quint64>(read32(bytes, 4)) + 8;
    if (riffEnd > static_cast<quint64>(bytes.size()) || riffEnd < 12)
    {
        setError(error, QStringLiteral("The WAV file has an invalid RIFF size."));
        return false;
    }

    bool foundFormat = false;
    bool foundData = false;
    quint16 format = 0;
    quint16 channels = 0;
    quint16 bitsPerSample = 0;
    quint16 blockAlign = 0;
    quint32 sampleRate = 0;
    int dataOffset = 0;
    quint32 dataBytes = 0;
    quint64 offset = 12;
    while (offset + 8 <= riffEnd)
    {
        const QByteArray chunkName = bytes.mid(static_cast<int>(offset), 4);
        const quint32 chunkSize = read32(bytes, static_cast<int>(offset) + 4);
        const quint64 chunkStart = offset + 8;
        const quint64 chunkEnd = chunkStart + chunkSize;
        const quint64 nextChunk = chunkEnd + (chunkSize & 1u);
        if (chunkEnd > riffEnd || nextChunk > riffEnd)
        {
            setError(error, QStringLiteral("A WAV chunk extends past the end of the RIFF container."));
            return false;
        }
        if (chunkName == QByteArrayLiteral("fmt "))
        {
            if (chunkSize < 16)
            {
                setError(error, QStringLiteral("The WAV format chunk is incomplete."));
                return false;
            }
            format = read16(bytes, static_cast<int>(chunkStart));
            channels = read16(bytes, static_cast<int>(chunkStart) + 2);
            sampleRate = read32(bytes, static_cast<int>(chunkStart) + 4);
            blockAlign = read16(bytes, static_cast<int>(chunkStart) + 12);
            bitsPerSample = read16(bytes, static_cast<int>(chunkStart) + 14);
            foundFormat = true;
        }
        else if (chunkName == QByteArrayLiteral("data") && !foundData)
        {
            dataOffset = static_cast<int>(chunkStart);
            dataBytes = chunkSize;
            foundData = true;
        }
        offset = nextChunk;
    }

    const bool pcmFormat = format == 1 &&
        (bitsPerSample == 8 || bitsPerSample == 16 || bitsPerSample == 24 || bitsPerSample == 32);
    const bool floatFormat = format == 3 && bitsPerSample == 32;
    const int bytesPerSample = bitsPerSample / 8;
    if (!foundFormat || !foundData || (!pcmFormat && !floatFormat) || channels < 1 || channels > 2 ||
        sampleRate < 8000 || sampleRate > 192000 || bytesPerSample == 0 ||
        blockAlign != channels * bytesPerSample || dataBytes == 0 || dataBytes % blockAlign != 0)
    {
        setError(error, QStringLiteral("Supported WAV audio is mono or stereo PCM/float32 at 8–192 kHz."));
        return false;
    }

    const quint64 frameCount = dataBytes / blockAlign;
    if (frameCount == 0 || frameCount * channels > maximumWavSamples)
    {
        setError(error, QStringLiteral("This WAV file contains too many samples to load into memory."));
        return false;
    }

    auto decoded = std::make_shared<AudioAsset>();
    decoded->path = QFileInfo(path).absoluteFilePath();
    decoded->sampleRate = static_cast<int>(sampleRate);
    decoded->channelCount = channels;
    decoded->samples.reserve(static_cast<qsizetype>(frameCount * channels));
    decoded->waveformPeaks.reserve(static_cast<qsizetype>(
        (frameCount + AudioAsset::waveformBlockFrames - 1) / AudioAsset::waveformBlockFrames));
    for (quint64 frame = 0; frame < frameCount; ++frame)
    {
        const int frameOffset = dataOffset + static_cast<int>(frame * blockAlign);
        float framePeak = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            const float sample = decodeSample(bytes, frameOffset + channel * bytesPerSample,
                                              format, bitsPerSample);
            const float clampedSample = std::clamp(sample, -4.0f, 4.0f);
            decoded->samples.push_back(clampedSample);
            framePeak = std::max(framePeak, std::abs(clampedSample));
        }
        const qsizetype peakIndex = static_cast<qsizetype>(frame / AudioAsset::waveformBlockFrames);
        if (peakIndex == decoded->waveformPeaks.size())
            decoded->waveformPeaks.push_back(framePeak);
        else
            decoded->waveformPeaks[peakIndex] = std::max(decoded->waveformPeaks[peakIndex], framePeak);
    }
    asset = std::move(decoded);
    return true;
}
