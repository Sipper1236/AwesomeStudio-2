#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QString>

#include <functional>
#include <memory>

class QAudioSource;
class QIODevice;
class QSaveFile;

class AudioRecorder
{
public:
    using MonitorCallback = std::function<void(const float*, qsizetype, int, int)>;

    AudioRecorder();
    ~AudioRecorder();

    bool start(const QAudioDevice& device, const QString& path,
               quint64 maximumDecodedBytes, QString* error = nullptr);
    bool finish(QString* error = nullptr);
    bool isActive() const;
    bool maximumReached() const;
    QString errorMessage() const;
    QString path() const;
    quint64 frameCount() const;
    int sampleRate() const;
    int channelCount() const;
    void setMonitorCallback(MonitorCallback callback);

private:
    bool chooseFormat(const QAudioDevice& device);
    void consumeAvailable();
    bool writeHeader(quint32 dataBytes);
    void setError(const QString& error);

    std::unique_ptr<QAudioSource> source;
    std::unique_ptr<QSaveFile> file;
    QIODevice* input = nullptr;
    QAudioFormat inputFormat;
    QString outputPath;
    QString failure;
    QByteArray pendingBytes;
    quint64 maximumFrames = 0;
    quint64 framesWritten = 0;
    int outputChannels = 0;
    bool reachedLimit = false;
    bool active = false;
    MonitorCallback monitorCallback;
};
