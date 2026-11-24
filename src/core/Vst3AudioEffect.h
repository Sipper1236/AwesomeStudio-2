#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

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
    qint64 processingLatencyFrames() const;
    qsizetype parameterCount() const;
    float parameterValue(qsizetype index) const;
    void setParameter(qsizetype index, float value);
    bool saveState(QString& pluginState) const;
    void stopProcessing();
    void resetProcessingState();
    bool process(float& left, float& right, float& alignedDryLeft, float& alignedDryRight);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
