#include "AudioLatencyCalibration.h"

#include <QAudioSink>
#include <QIODevice>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
constexpr qsizetype maximumCaptureSeconds = 3;
constexpr qsizetype probeFrameCount = 1023;
constexpr int calibrationRepeatCount = 3;
constexpr int calibrationRepeatIntervalMs = 750;
constexpr int maximumRoundTripMs = 500;
constexpr float probeAmplitude = 0.45f;
constexpr float minimumCorrelation = 0.08f;

void writeSample(char* destination, QAudioFormat::SampleFormat format, float sample)
{
    sample = std::clamp(sample, -1.0f, 1.0f);
    switch (format)
    {
        case QAudioFormat::Int16:
        {
            const qint16 value = static_cast<qint16>(std::lrint(sample * 32767.0f));
            std::memcpy(destination, &value, sizeof(value));
            break;
        }
        case QAudioFormat::Int32:
        {
            const qint32 value = static_cast<qint32>(std::llround(
                static_cast<double>(sample) * 2147483647.0));
            std::memcpy(destination, &value, sizeof(value));
            break;
        }
        case QAudioFormat::Float:
            std::memcpy(destination, &sample, sizeof(sample));
            break;
        default:
            break;
    }
}
}

AudioLatencyCalibration::AudioLatencyCalibration(QObject* parent)
    : QObject(parent), timeout(std::make_unique<QTimer>())
{
    timeout->setSingleShot(true);
    connect(timeout.get(), &QTimer::timeout, this, [this]
    {
        if (probeStarted)
            finishProbe();
        else
            complete(false, 0.0, tr("No audio data arrived from the selected input."));
    });
}

AudioLatencyCalibration::~AudioLatencyCalibration()
{
    cancel();
}

bool AudioLatencyCalibration::start(const QAudioDevice& inputDevice,
                                    const QAudioDevice& outputDevice,
                                    Completion onComplete, QString* error)
{
    if (capture.isActive() || sink != nullptr)
    {
        if (error != nullptr)
            *error = tr("A latency calibration is already running.");
        return false;
    }
    if (inputDevice.isNull() || outputDevice.isNull() || !onComplete)
    {
        if (error != nullptr)
            *error = tr("Select an audio input and output device before calibrating.");
        return false;
    }

    completion = std::move(onComplete);
    this->outputDevice = outputDevice;
    capturedMono.clear();
    probe.clear();
    captureSampleRate = 0;
    outputStartCaptureFrame = -1;
    silenceFrames = 0;
    probeStarted = false;

    QString captureError;
    const auto callback = [this](const float* samples, qsizetype frames, int channels, int sampleRate)
    {
        receiveInput(samples, frames, channels, sampleRate);
    };
    if (!capture.startMonitoring(inputDevice, callback, &captureError))
    {
        completion = {};
        if (error != nullptr)
            *error = captureError;
        return false;
    }
    timeout->start(8000);
    return true;
}

bool AudioLatencyCalibration::isActive() const
{
    return capture.isActive() || sink != nullptr;
}

void AudioLatencyCalibration::cancel()
{
    if (timeout != nullptr)
        timeout->stop();
    if (sink != nullptr)
        sink->stop();
    sink.reset();
    sinkDevice = nullptr;
    if (capture.isActive())
    {
        capture.setMonitorCallback({});
        QString ignoredError;
        capture.finish(&ignoredError);
    }
    completion = {};
    capturedMono.clear();
    probe.clear();
    probeStarted = false;
}

void AudioLatencyCalibration::receiveInput(const float* samples, qsizetype frames,
                                           int channels, int sampleRate)
{
    if (samples == nullptr || frames <= 0 || channels <= 0 || sampleRate <= 0 ||
        (probeStarted && sampleRate != captureSampleRate))
        return;
    if (captureSampleRate == 0)
    {
        captureSampleRate = sampleRate;
        capturedMono.reserve(static_cast<qsizetype>(sampleRate) * maximumCaptureSeconds);
        sinkFormat = outputFormatFor(outputDevice, sampleRate);
        if (!sinkFormat.isValid())
        {
            QTimer::singleShot(0, this, [this]
            {
                complete(false, 0.0,
                         tr("The selected input and output do not share a supported sample rate."));
            });
            return;
        }
        probe.resize(probeFrameCount);
        quint32 state = 0x6d2b79f5u;
        for (float& value : probe)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            value = (state & 1u) != 0 ? probeAmplitude : -probeAmplitude;
        }
    }
    if (sampleRate != captureSampleRate)
        return;

    const qsizetype available = static_cast<qsizetype>(sampleRate) * maximumCaptureSeconds -
                                capturedMono.size();
    const qsizetype accepted = std::min(frames, available);
    for (qsizetype frame = 0; frame < accepted; ++frame)
    {
        float mono = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
            mono += samples[frame * channels + channel] / channels;
        capturedMono.append(mono);
    }

    if (!probeStarted && capturedMono.size() >= sampleRate / 2)
    {
        QString error;
        if (!startProbe(&error))
            QTimer::singleShot(0, this, [this, error]
            {
                complete(false, 0.0, error);
            });
    }
}

bool AudioLatencyCalibration::startProbe(QString* error)
{
    sink = std::make_unique<QAudioSink>(outputDevice, sinkFormat);
    sink->setVolume(0.3f);
    const qsizetype outputFrames = static_cast<qsizetype>(captureSampleRate) * 3;
    const qint64 requestedBuffer = sinkFormat.bytesForFrames(outputFrames);
    if (requestedBuffer > 0)
        sink->setBufferSize(requestedBuffer);
    sinkDevice = sink->start();
    if (sinkDevice == nullptr ||
        (sink->state() == QAudio::StoppedState && sink->error() != QAudio::NoError))
    {
        if (error != nullptr)
            *error = tr("Could not start audio output for the calibration probe (error %1).")
                         .arg(static_cast<int>(sink->error()));
        sink->stop();
        sink.reset();
        return false;
    }

    silenceFrames = captureSampleRate / 5;
    const qsizetype trailingFrames = captureSampleRate / 5;
    const qsizetype repeatIntervalFrames = static_cast<qsizetype>(captureSampleRate) *
                                          calibrationRepeatIntervalMs / 1000;
    const qsizetype totalFrames = silenceFrames +
        (calibrationRepeatCount - 1) * repeatIntervalFrames + probe.size() + trailingFrames;
    const qsizetype bytesPerFrame = sinkFormat.bytesPerFrame();
    if (bytesPerFrame <= 0 || totalFrames > std::numeric_limits<int>::max() / bytesPerFrame)
    {
        if (error != nullptr)
            *error = tr("The audio output returned an unsupported calibration format.");
        sink->stop();
        sink.reset();
        return false;
    }

    QByteArray output(static_cast<int>(totalFrames * bytesPerFrame), '\0');
    const int bytesPerSample = sinkFormat.bytesPerSample();
    for (int repeat = 0; repeat < calibrationRepeatCount; ++repeat)
    {
        const qsizetype repeatStart = silenceFrames + repeat * repeatIntervalFrames;
        for (qsizetype frame = 0; frame < probe.size(); ++frame)
        {
            const float value = probe[frame];
            for (int channel = 0; channel < sinkFormat.channelCount(); ++channel)
            {
                char* destination = output.data() +
                    (repeatStart + frame) * bytesPerFrame + channel * bytesPerSample;
                writeSample(destination, sinkFormat.sampleFormat(), value);
            }
        }
    }

    outputStartCaptureFrame = capturedMono.size();
    const qint64 written = sinkDevice->write(output.constData(), output.size());
    if (written != output.size())
    {
        if (error != nullptr)
            *error = tr("Could not queue the complete audio calibration probe.");
        sink->stop();
        sink.reset();
        sinkDevice = nullptr;
        return false;
    }
    probeStarted = true;
    timeout->start(2500);
    return true;
}

void AudioLatencyCalibration::finishProbe()
{
    if (!probeStarted || !capture.isActive())
        return;
    const qsizetype repeatIntervalFrames = static_cast<qsizetype>(captureSampleRate) *
                                           calibrationRepeatIntervalMs / 1000;
    QVector<double> roundTripMeasurements;
    for (int repeat = 0; repeat < calibrationRepeatCount; ++repeat)
    {
        const qsizetype searchStart = outputStartCaptureFrame + silenceFrames +
                                      repeat * repeatIntervalFrames;
        const qsizetype searchEnd = std::min(
            capturedMono.size() - probe.size(),
            searchStart + static_cast<qsizetype>(captureSampleRate) * maximumRoundTripMs / 1000);
        if (searchStart < 0 || searchEnd < searchStart)
            continue;

        double bestCorrelation = 0.0;
        qsizetype bestFrame = -1;
        for (qsizetype startFrame = searchStart; startFrame <= searchEnd; ++startFrame)
        {
            double dot = 0.0;
            double energy = 0.0;
            for (qsizetype index = 0; index < probe.size(); ++index)
            {
                const double sample = capturedMono[startFrame + index];
                dot += sample * probe[index];
                energy += sample * sample;
            }
            if (energy <= 1.0e-10)
                continue;
            const double correlation = std::abs(dot) / std::sqrt(energy * probe.size());
            if (correlation > bestCorrelation)
            {
                bestCorrelation = correlation;
                bestFrame = startFrame;
            }
        }
        if (bestFrame >= 0 && bestCorrelation >= minimumCorrelation)
            roundTripMeasurements.append(static_cast<double>(bestFrame - searchStart) *
                                         1000.0 / captureSampleRate);
    }

    if (roundTripMeasurements.size() < 2)
    {
        complete(false, 0.0,
                 tr("Fewer than two calibration probes were detected clearly. Check the cable, input, and output levels."));
        return;
    }

    QString captureError;
    capture.setMonitorCallback({});
    if (!capture.finish(&captureError))
    {
        complete(false, 0.0, captureError);
        return;
    }
    std::sort(roundTripMeasurements.begin(), roundTripMeasurements.end());
    const qsizetype middle = roundTripMeasurements.size() / 2;
    const double roundTripMs = roundTripMeasurements.size() % 2 == 0
        ? (roundTripMeasurements[middle - 1] + roundTripMeasurements[middle]) * 0.5
        : roundTripMeasurements[middle];
    const double inputBufferEstimateMs = capture.estimatedInputLatencyMs().value_or(0.0);
    complete(true, std::max(0.0, roundTripMs - inputBufferEstimateMs));
}

void AudioLatencyCalibration::complete(bool success, double compensationMs, const QString& error)
{
    if (timeout != nullptr)
        timeout->stop();
    if (sink != nullptr)
        sink->stop();
    sink.reset();
    sinkDevice = nullptr;
    if (capture.isActive())
    {
        capture.setMonitorCallback({});
        QString ignoredError;
        capture.finish(&ignoredError);
    }
    probeStarted = false;
    Completion callback = std::move(completion);
    capturedMono.clear();
    probe.clear();
    if (callback)
        callback(success, compensationMs, error);
}

QAudioFormat AudioLatencyCalibration::outputFormatFor(const QAudioDevice& device, int sampleRate)
{
    const QList<int> channelCounts = {2, 1};
    const QList<QAudioFormat::SampleFormat> sampleFormats = {
        QAudioFormat::Int16, QAudioFormat::Float, QAudioFormat::Int32
    };
    for (int channels : channelCounts)
    {
        for (QAudioFormat::SampleFormat sampleFormat : sampleFormats)
        {
            QAudioFormat format;
            format.setSampleRate(sampleRate);
            format.setChannelCount(channels);
            format.setSampleFormat(sampleFormat);
            if (device.isFormatSupported(format))
                return format;
        }
    }
    return {};
}
