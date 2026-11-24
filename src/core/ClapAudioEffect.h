#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

enum class ClapPluginKind
{
    AudioEffect,
    Instrument
};

struct ClapPluginParameter
{
    std::uint32_t id = 0;
    QString name;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    bool integer = false;
    bool toggled = false;
    bool hidden = false;
    bool readOnly = false;
};

struct ClapPluginDescriptor
{
    QString id;
    QString name;
    QString vendor;
    QString path;
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    bool hasState = false;
    bool supportsMidiDialect = false;
    QVector<ClapPluginParameter> parameters;
};

class ClapAudioPluginInstance
{
public:
    static QVector<ClapPluginDescriptor> availablePlugins(ClapPluginKind kind);
    static bool describe(const QString& id, ClapPluginKind kind,
                         ClapPluginDescriptor& descriptor);

    ClapAudioPluginInstance(const QString& id, ClapPluginKind kind, double sampleRate,
                            const QVector<float>& parameterValues,
                            const QString& pluginState = QString());
    ~ClapAudioPluginInstance();

    ClapAudioPluginInstance(const ClapAudioPluginInstance&) = delete;
    ClapAudioPluginInstance& operator=(const ClapAudioPluginInstance&) = delete;

    bool isValid() const;
    qint64 processingLatencyFrames() const;
    qsizetype parameterCount() const;
    float parameterValue(qsizetype index) const;
    void setParameter(qsizetype index, float value);
    void noteOn(std::int32_t noteId, int channel, int key, float velocity);
    void noteOff(std::int32_t noteId, int channel, int key, float velocity = 0.0f);
    void allNotesOff();
    void pitchBend(int channel, int value);
    bool saveState(QString& pluginState) const;
    void serviceMainThread();
    void stopProcessing();
    void resetProcessingState();
    bool process(float& left, float& right);
    bool process(float& left, float& right, float& alignedDryLeft, float& alignedDryRight);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
