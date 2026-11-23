#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

struct ClapEffectParameter
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

struct ClapEffectDescriptor
{
    QString id;
    QString name;
    QString vendor;
    QString path;
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    bool hasState = false;
    QVector<ClapEffectParameter> parameters;
};

class ClapAudioEffectInstance
{
public:
    static QVector<ClapEffectDescriptor> availableEffects();
    static bool describe(const QString& id, ClapEffectDescriptor& descriptor);

    ClapAudioEffectInstance(const QString& id, double sampleRate,
                            const QVector<float>& parameterValues,
                            const QString& pluginState = QString());
    ~ClapAudioEffectInstance();

    ClapAudioEffectInstance(const ClapAudioEffectInstance&) = delete;
    ClapAudioEffectInstance& operator=(const ClapAudioEffectInstance&) = delete;

    bool isValid() const;
    qsizetype parameterCount() const;
    void setParameter(qsizetype index, float value);
    bool saveState(QString& pluginState) const;
    void serviceMainThread();
    void stopProcessing();
    bool process(float& left, float& right);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
