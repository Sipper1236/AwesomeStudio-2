#pragma once

#include "AudioRecorder.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QObject>
#include <QVector>

#include <functional>
#include <memory>

class QAudioSink;
class QIODevice;
class QTimer;

class AudioLatencyCalibration final : public QObject
{
public:
    using Completion = std::function<void(bool, double, const QString&)>;

    explicit AudioLatencyCalibration(QObject* parent = nullptr);
    ~AudioLatencyCalibration() override;

    bool start(const QAudioDevice& inputDevice, const QAudioDevice& outputDevice,
               Completion completion, QString* error = nullptr);
    bool isActive() const;
    void cancel();

private:
    void receiveInput(const float* samples, qsizetype frames, int channels, int sampleRate);
    bool startProbe(QString* error);
    void finishProbe();
    void complete(bool success, double compensationMs, const QString& error = {});
    static QAudioFormat outputFormatFor(const QAudioDevice& device, int sampleRate);

    AudioRecorder capture;
    std::unique_ptr<QAudioSink> sink;
    QIODevice* sinkDevice = nullptr;
    std::unique_ptr<QTimer> timeout;
    Completion completion;
    QVector<float> capturedMono;
    QVector<float> probe;
    QAudioDevice outputDevice;
    QAudioFormat sinkFormat;
    qsizetype outputStartCaptureFrame = -1;
    qsizetype silenceFrames = 0;
    int captureSampleRate = 0;
    bool probeStarted = false;
};
