#pragma once

#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <vector>

struct Lv2EffectParameter
{
    QString symbol;
    QString name;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float defaultValue = 0.0f;
    bool integer = false;
    bool toggled = false;
    std::uint32_t portIndex = 0;
};

struct Lv2EffectDescriptor
{
    QString uri;
    QString name;
    bool hardRealtime = false;
    QVector<Lv2EffectParameter> parameters;
    std::vector<std::uint32_t> audioInputs;
    std::vector<std::uint32_t> audioOutputs;
    std::vector<std::uint32_t> controlOutputs;
};

class Lv2AudioEffectInstance
{
public:
    static QVector<Lv2EffectDescriptor> availableEffects();
    static bool describe(const QString& uri, Lv2EffectDescriptor& descriptor);

    Lv2AudioEffectInstance(const QString& uri, double sampleRate,
                           const QVector<float>& parameterValues,
                           const QString& pluginState = QString());
    ~Lv2AudioEffectInstance();

    Lv2AudioEffectInstance(const Lv2AudioEffectInstance&) = delete;
    Lv2AudioEffectInstance& operator=(const Lv2AudioEffectInstance&) = delete;

    bool isValid() const;
    qsizetype parameterCount() const;
    void setParameter(qsizetype index, float value);
    bool saveState(QString& pluginState) const;
    void process(float& left, float& right);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
