#include "AudioRecorder.h"

#include "AudioFile.h"

#include <QAudioSource>
#include <QIODevice>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
constexpr quint64 maximumWavBytes = 128ULL * 1024 * 1024;
constexpr quint64 maximumWavSamples = 40000000ULL;
constexpr quint64 wavHeaderBytes = 44;

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

quint64 decodedBytesForFrames(quint64 frames, int channels)
{
    return frames * static_cast<quint64>(channels) * sizeof(float) +
           ((frames + AudioAsset::waveformBlockFrames - 1) / AudioAsset::waveformBlockFrames) * sizeof(float);
}

quint64 frameLimitForMemory(quint64 bytes, int channels)
{
    quint64 low = 0;
    quint64 high = maximumWavSamples / static_cast<quint64>(channels);
    while (low < high)
    {
        const quint64 middle = low + (high - low + 1) / 2;
        if (decodedBytesForFrames(middle, channels) <= bytes)
            low = middle;
        else
            high = middle - 1;
    }
    return low;
}
}

AudioRecorder::AudioRecorder() = default;

AudioRecorder::~AudioRecorder()
{
    if (source)
        source->stop();
    if (file)
        file->cancelWriting();
}

bool AudioRecorder::start(const QAudioDevice& device, const QString& path,
                          quint64 maximumDecodedBytes, QString* error)
{
    if (active)
    {
        if (error != nullptr)
            *error = QStringLiteral("A recording is already active.");
        return false;
    }
    if (device.isNull() || !chooseFormat(device))
    {
        if (error != nullptr)
            *error = QStringLiteral("The selected input device has no supported mono or stereo WAV format.");
        return false;
    }

    maximumFrames = std::min(
        (maximumWavBytes - wavHeaderBytes) / static_cast<quint64>(outputChannels * 2),
        frameLimitForMemory(maximumDecodedBytes, outputChannels));
    if (maximumFrames == 0)
    {
        if (error != nullptr)
            *error = QStringLiteral("There is not enough free project audio memory for a recording.");
        return false;
    }

    outputPath = path;
    file = std::make_unique<QSaveFile>(path);
    if (!file->open(QIODevice::WriteOnly))
    {
        if (error != nullptr)
            *error = file->errorString();
        file.reset();
        return false;
    }
    if (!writeHeader(0))
    {
        if (error != nullptr)
            *error = file->errorString();
        file->cancelWriting();
        file.reset();
        return false;
    }

    source = std::make_unique<QAudioSource>(device, inputFormat);
    const qint64 requestedBuffer = inputFormat.bytesForDuration(200000);
    if (requestedBuffer > 0)
        source->setBufferSize(static_cast<qsizetype>(requestedBuffer));
    input = source->start();
    if (input == nullptr || (source->state() == QAudio::StoppedState && source->error() != QAudio::NoError))
    {
        const QString message = QStringLiteral("Could not start audio input (error %1).")
                                    .arg(static_cast<int>(source->error()));
        source->stop();
        source.reset();
        file->cancelWriting();
        file.reset();
        if (error != nullptr)
            *error = message;
        return false;
    }

    QObject::connect(input, &QIODevice::readyRead, source.get(), [this] { consumeAvailable(); });
    failure.clear();
    framesWritten = 0;
    reachedLimit = false;
    pendingBytes.clear();
    active = true;
    return true;
}

bool AudioRecorder::finish(QString* error)
{
    if (!file)
    {
        if (error != nullptr)
            *error = failure;
        return false;
    }

    consumeAvailable();
    if (source)
    {
        source->stop();
        source.reset();
    }
    input = nullptr;
    active = false;

    if (!failure.isEmpty() || framesWritten == 0)
    {
        const QString message = !failure.isEmpty()
            ? failure : QStringLiteral("No audio frames were captured.");
        file->cancelWriting();
        file.reset();
        if (error != nullptr)
            *error = message;
        return false;
    }

    const quint64 dataBytes = framesWritten * static_cast<quint64>(outputChannels * 2);
    if (!file->seek(0) || !writeHeader(static_cast<quint32>(dataBytes)) || !file->commit())
    {
        const QString message = file->errorString();
        file->cancelWriting();
        file.reset();
        if (error != nullptr)
            *error = message;
        return false;
    }
    file.reset();
    return true;
}

bool AudioRecorder::isActive() const
{
    return active;
}

bool AudioRecorder::maximumReached() const
{
    return reachedLimit;
}

QString AudioRecorder::errorMessage() const
{
    if (!failure.isEmpty())
        return failure;
    if (source && source->error() != QAudio::NoError)
        return QStringLiteral("Audio input stopped with error %1.").arg(static_cast<int>(source->error()));
    return {};
}

QString AudioRecorder::path() const
{
    return outputPath;
}

quint64 AudioRecorder::frameCount() const
{
    return framesWritten;
}

int AudioRecorder::sampleRate() const
{
    return inputFormat.sampleRate();
}

int AudioRecorder::channelCount() const
{
    return outputChannels;
}

bool AudioRecorder::chooseFormat(const QAudioDevice& device)
{
    const QAudioFormat preferred = device.preferredFormat();
    const QList<QAudioFormat::SampleFormat> sampleFormats = {
        QAudioFormat::Int16, QAudioFormat::Float, QAudioFormat::Int32, QAudioFormat::UInt8
    };
    const QList<int> sampleRates = {48000, 44100, 96000, 32000, 22050, 192000, 16000, 8000};
    const QList<int> channelCounts = {2, 1};

    for (int channels : channelCounts)
    {
        for (int rate : sampleRates)
        {
            for (QAudioFormat::SampleFormat sampleFormat : sampleFormats)
            {
                QAudioFormat candidate;
                candidate.setSampleRate(rate);
                candidate.setChannelCount(channels);
                candidate.setSampleFormat(sampleFormat);
                if (device.isFormatSupported(candidate))
                {
                    inputFormat = candidate;
                    outputChannels = channels;
                    return true;
                }
            }
        }
    }

    if (preferred.isValid() && preferred.sampleRate() >= 8000 && preferred.sampleRate() <= 192000 &&
        preferred.channelCount() >= 1 && preferred.channelCount() <= 8 &&
        preferred.bytesPerFrame() > 0 && device.isFormatSupported(preferred))
    {
        inputFormat = preferred;
        outputChannels = preferred.channelCount() == 1 ? 1 : 2;
        return true;
    }
    return false;
}

void AudioRecorder::consumeAvailable()
{
    if (input == nullptr || file == nullptr || !failure.isEmpty() || reachedLimit)
        return;

    pendingBytes.append(input->readAll());
    const int inputBytesPerFrame = inputFormat.bytesPerFrame();
    if (inputBytesPerFrame <= 0)
    {
        setError(QStringLiteral("The audio input returned an invalid frame format."));
        return;
    }

    const quint64 availableFrames = static_cast<quint64>(pendingBytes.size() / inputBytesPerFrame);
    const quint64 remainingFrames = maximumFrames - framesWritten;
    const quint64 frames = std::min(availableFrames, remainingFrames);
    if (frames == 0)
    {
        if (remainingFrames == 0)
            reachedLimit = true;
        return;
    }

    const quint64 outputByteCount = frames * static_cast<quint64>(outputChannels * 2);
    if (outputByteCount > static_cast<quint64>(std::numeric_limits<int>::max()))
    {
        setError(QStringLiteral("The audio input buffer exceeded the safe recording chunk size."));
        return;
    }
    QByteArray output(static_cast<int>(outputByteCount), Qt::Uninitialized);
    const MonitorCallback monitor = monitorCallback;
    QVector<float> monitorSamples;
    if (monitor)
        monitorSamples.resize(static_cast<qsizetype>(frames * static_cast<quint64>(outputChannels)));
    const int inputChannels = inputFormat.channelCount();
    const int inputBytesPerSample = inputFormat.bytesPerSample();
    for (quint64 frame = 0; frame < frames; ++frame)
    {
        const char* inputFrame = pendingBytes.constData() +
            static_cast<qsizetype>(frame * static_cast<quint64>(inputBytesPerFrame));
        for (int channel = 0; channel < outputChannels; ++channel)
        {
            float sample = 0.0f;
            if (outputChannels == 1 && inputChannels > 1)
            {
                for (int inputChannel = 0; inputChannel < inputChannels; ++inputChannel)
                {
                    const char* inputSample = inputFrame + inputChannel * inputBytesPerSample;
                    sample += inputFormat.normalizedSampleValue(inputSample) / inputChannels;
                }
            }
            else
            {
                const int inputChannel = inputChannels == 1 ? 0 : std::min(channel, inputChannels - 1);
                sample = inputFormat.normalizedSampleValue(inputFrame + inputChannel * inputBytesPerSample);
            }
            const qint16 value = static_cast<qint16>(std::lrint(std::clamp(sample, -1.0f, 1.0f) * 32767.0f));
            const qsizetype outputOffset = static_cast<qsizetype>(frame * outputChannels * 2 + channel * 2);
            write16(output.data() + outputOffset, static_cast<quint16>(value));
            if (monitor)
                monitorSamples[static_cast<qsizetype>(frame * outputChannels + channel)] = sample;
        }
    }

    if (file->write(output.constData(), output.size()) != output.size())
    {
        setError(file->errorString());
        return;
    }
    framesWritten += frames;
    pendingBytes.remove(0, static_cast<qsizetype>(frames * inputBytesPerFrame));
    if (monitor)
        monitor(monitorSamples.constData(), static_cast<qsizetype>(frames), outputChannels,
                inputFormat.sampleRate());
    if (framesWritten >= maximumFrames)
        reachedLimit = true;
}

void AudioRecorder::setMonitorCallback(MonitorCallback callback)
{
    monitorCallback = std::move(callback);
}

bool AudioRecorder::writeHeader(quint32 dataBytes)
{
    char header[44]{};
    std::memcpy(header, "RIFF", 4);
    write32(header + 4, dataBytes + 36);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    write32(header + 16, 16);
    write16(header + 20, 1);
    write16(header + 22, static_cast<quint16>(outputChannels));
    write32(header + 24, static_cast<quint32>(inputFormat.sampleRate()));
    const quint32 bytesPerFrame = static_cast<quint32>(outputChannels * 2);
    write32(header + 28, static_cast<quint32>(inputFormat.sampleRate()) * bytesPerFrame);
    write16(header + 32, static_cast<quint16>(bytesPerFrame));
    write16(header + 34, 16);
    std::memcpy(header + 36, "data", 4);
    write32(header + 40, dataBytes);
    return file->write(header, sizeof(header)) == sizeof(header);
}

void AudioRecorder::setError(const QString& error)
{
    if (failure.isEmpty())
        failure = error;
}
