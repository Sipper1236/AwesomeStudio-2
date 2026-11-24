#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

struct Vst3TransportContext
{
    qint64 projectTimeSamples = 0;
    double projectTimeMusic = 0.0;
    double tempo = 120.0;
    double sampleRate = 44100.0;
    double cycleStartMusic = 0.0;
    double cycleEndMusic = 0.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    bool playing = false;
    bool cycleActive = false;
};

struct Vst3PluginParameter
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
    QVector<float> plainValuesByNormalized;
};

struct Vst3PluginDescriptor
{
    QString id;
    QString name;
    QString vendor;
    QString path;
    bool instrument = false;
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    QVector<Vst3PluginParameter> parameters;
};

class Vst3AudioEffectInstance
{
public:
    static QVector<Vst3PluginDescriptor> availableEffects();
    static bool describe(const QString& id, Vst3PluginDescriptor& descriptor);

    Vst3AudioEffectInstance(const QString& id, double sampleRate,
                            const QVector<float>& parameterValues,
                            const QString& pluginState = QString());
    ~Vst3AudioEffectInstance();

    Vst3AudioEffectInstance(const Vst3AudioEffectInstance&) = delete;
    Vst3AudioEffectInstance& operator=(const Vst3AudioEffectInstance&) = delete;

    bool isValid() const;
    bool hasPendingLatencyRestart() const;
    bool serviceLatencyRestart();
    qint64 processingLatencyFrames() const;
    qsizetype parameterCount() const;
    float parameterValue(qsizetype index) const;
    void setParameter(qsizetype index, float value);
    bool saveState(QString& pluginState) const;
    void stopProcessing();
    void resetProcessingState();
    bool process(float& left, float& right, float& alignedDryLeft, float& alignedDryRight,
                 const Vst3TransportContext& transportContext);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class Vst3AudioInstrumentInstance
{
public:
    static QVector<Vst3PluginDescriptor> availableInstruments();
    static bool describe(const QString& id, Vst3PluginDescriptor& descriptor);

    Vst3AudioInstrumentInstance(const QString& id, double sampleRate,
                                const QVector<float>& parameterValues,
                                const QString& pluginState = QString());
    ~Vst3AudioInstrumentInstance();

    Vst3AudioInstrumentInstance(const Vst3AudioInstrumentInstance&) = delete;
    Vst3AudioInstrumentInstance& operator=(const Vst3AudioInstrumentInstance&) = delete;

    bool isValid() const;
    bool hasPendingLatencyRestart() const;
    bool serviceLatencyRestart();
    qint64 processingLatencyFrames() const;
    qsizetype parameterCount() const;
    float parameterValue(qsizetype index) const;
    void setParameter(qsizetype index, float value);
    bool saveState(QString& pluginState) const;
    void stopProcessing();
    void resetProcessingState();
    void allNotesOff();
    void noteOn(std::int32_t noteId, int channel, int key, float velocity);
    void noteOff(std::int32_t noteId, int channel, int key, float velocity = 0.0f);
    void pitchBend(int channel, int value);
    void notePressure(std::int32_t noteId, int channel, int key, float value);
    void setTransportContext(const Vst3TransportContext& transportContext);
    bool process(float& left, float& right,
                 const Vst3TransportContext& transportContext);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
