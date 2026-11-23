#include "Lv2AudioEffect.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#if defined(AWESOME_STUDIO_HAS_LILV)
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#endif

namespace
{
#if defined(AWESOME_STUDIO_HAS_LILV)
struct LilvWorldState
{
    LilvWorldState()
    {
        world = lilv_world_new();
        if (world == nullptr)
            return;
        lilv_world_load_all(world);
        audioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
        controlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
        inputPort = lilv_new_uri(world, LV2_CORE__InputPort);
        outputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
        integerProperty = lilv_new_uri(world, LV2_CORE__integer);
        toggledProperty = lilv_new_uri(world, LV2_CORE__toggled);
    }

    ~LilvWorldState()
    {
        lilv_node_free(audioPort);
        lilv_node_free(controlPort);
        lilv_node_free(inputPort);
        lilv_node_free(outputPort);
        lilv_node_free(integerProperty);
        lilv_node_free(toggledProperty);
        lilv_world_free(world);
    }

    LilvWorld* world = nullptr;
    std::mutex mutex;
    LilvNode* audioPort = nullptr;
    LilvNode* controlPort = nullptr;
    LilvNode* inputPort = nullptr;
    LilvNode* outputPort = nullptr;
    LilvNode* integerProperty = nullptr;
    LilvNode* toggledProperty = nullptr;
};

LilvWorldState& lilvState()
{
    static LilvWorldState state;
    return state;
}

float numberFromNode(const LilvNode* node, float fallback)
{
    if (node == nullptr)
        return fallback;
    if (lilv_node_is_float(node) || lilv_node_is_int(node))
        return lilv_node_as_float(node);
    return fallback;
}

bool describePlugin(const LilvPlugin* plugin, Lv2EffectDescriptor& descriptor)
{
    LilvWorldState& state = lilvState();
    if (plugin == nullptr || state.world == nullptr)
        return false;

    LilvNodes* requiredFeatures = lilv_plugin_get_required_features(plugin);
    const bool hasRequiredFeatures = requiredFeatures != nullptr &&
        lilv_nodes_size(requiredFeatures) > 0;
    lilv_nodes_free(requiredFeatures);
    if (hasRequiredFeatures)
        return false;
    LilvNode* realtimeFeature = lilv_new_uri(state.world, LV2_CORE__hardRTCapable);
    descriptor.hardRealtime = realtimeFeature != nullptr &&
        lilv_plugin_has_feature(plugin, realtimeFeature);
    lilv_node_free(realtimeFeature);
    if (!descriptor.hardRealtime)
        return false;

    const LilvNode* uriNode = lilv_plugin_get_uri(plugin);
    const LilvNode* nameNode = lilv_plugin_get_name(plugin);
    descriptor.uri = uriNode == nullptr ? QString() :
        QString::fromUtf8(lilv_node_as_string(uriNode));
    descriptor.name = nameNode == nullptr ? descriptor.uri :
        QString::fromUtf8(lilv_node_as_string(nameNode));
    if (descriptor.uri.isEmpty())
        return false;

    const uint32_t portCount = lilv_plugin_get_num_ports(plugin);
    for (uint32_t portIndex = 0; portIndex < portCount; ++portIndex)
    {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, portIndex);
        if (port == nullptr)
            return false;
        const bool isInput = lilv_port_is_a(plugin, port, state.inputPort);
        const bool isOutput = lilv_port_is_a(plugin, port, state.outputPort);
        const bool isAudio = lilv_port_is_a(plugin, port, state.audioPort);
        const bool isControl = lilv_port_is_a(plugin, port, state.controlPort);
        if (isAudio)
        {
            if (isInput)
                descriptor.audioInputs.push_back(portIndex);
            if (isOutput)
                descriptor.audioOutputs.push_back(portIndex);
            continue;
        }
        if (isControl)
        {
            if (isOutput)
            {
                descriptor.controlOutputs.push_back(portIndex);
                continue;
            }
            if (!isInput)
                continue;

            const LilvNode* symbolNode = lilv_port_get_symbol(plugin, port);
            const LilvNode* portName = lilv_port_get_name(plugin, port);
            LilvNode* defaultNode = nullptr;
            LilvNode* minimumNode = nullptr;
            LilvNode* maximumNode = nullptr;
            lilv_port_get_range(plugin, port, &defaultNode, &minimumNode, &maximumNode);
            float minimum = numberFromNode(minimumNode, 0.0f);
            float maximum = numberFromNode(maximumNode, 1.0f);
            if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
            {
                minimum = 0.0f;
                maximum = 1.0f;
            }
            float defaultValue = numberFromNode(defaultNode, minimum);
            if (!std::isfinite(defaultValue))
                defaultValue = minimum;
            defaultValue = std::clamp(defaultValue, minimum, maximum);
            Lv2EffectParameter parameter;
            parameter.symbol = symbolNode == nullptr ? QString::number(portIndex) :
                QString::fromUtf8(lilv_node_as_string(symbolNode));
            parameter.name = portName == nullptr ? parameter.symbol :
                QString::fromUtf8(lilv_node_as_string(portName));
            parameter.minimum = minimum;
            parameter.maximum = maximum;
            parameter.defaultValue = defaultValue;
            parameter.integer = state.integerProperty != nullptr &&
                lilv_port_has_property(plugin, port, state.integerProperty);
            parameter.toggled = state.toggledProperty != nullptr &&
                lilv_port_has_property(plugin, port, state.toggledProperty);
            parameter.portIndex = portIndex;
            descriptor.parameters.push_back(parameter);
            lilv_node_free(defaultNode);
            lilv_node_free(minimumNode);
            lilv_node_free(maximumNode);
            continue;
        }

        // MIDI, Atom, CV, and other event ports need additional host features.
        if (isInput || isOutput)
            return false;
    }

    return !descriptor.audioInputs.empty() && !descriptor.audioOutputs.empty() &&
           descriptor.audioInputs.size() <= 2 && descriptor.audioOutputs.size() <= 2;
}
#endif
}

struct Lv2AudioEffectInstance::Private
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    LilvInstance* instance = nullptr;
    Lv2EffectDescriptor descriptor;
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float monoInput = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    float monoOutput = 0.0f;
    std::vector<float> controlInputs;
    std::vector<float> controlOutputs;
#endif
};

QVector<Lv2EffectDescriptor> Lv2AudioEffectInstance::availableEffects()
{
    QVector<Lv2EffectDescriptor> result;
#if defined(AWESOME_STUDIO_HAS_LILV)
    LilvWorldState& state = lilvState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.world == nullptr)
        return result;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(state.world);
    for (LilvIter* iterator = lilv_plugins_begin(plugins);
         !lilv_plugins_is_end(plugins, iterator);
         iterator = lilv_plugins_next(plugins, iterator))
    {
        const LilvPlugin* plugin = lilv_plugins_get(plugins, iterator);
        Lv2EffectDescriptor descriptor;
        if (describePlugin(plugin, descriptor))
            result.push_back(std::move(descriptor));
    }
#endif
    return result;
}

bool Lv2AudioEffectInstance::describe(const QString& uri, Lv2EffectDescriptor& descriptor)
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    LilvWorldState& state = lilvState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.world == nullptr || uri.isEmpty())
        return false;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(state.world);
    const QByteArray uriBytes = uri.toUtf8();
    LilvNode* uriNode = lilv_new_uri(state.world, uriBytes.constData());
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uriNode);
    lilv_node_free(uriNode);
    Lv2EffectDescriptor described;
    if (!describePlugin(plugin, described))
        return false;
    descriptor = std::move(described);
    return true;
#else
    Q_UNUSED(uri)
    Q_UNUSED(descriptor)
    return false;
#endif
}

Lv2AudioEffectInstance::Lv2AudioEffectInstance(const QString& uri, double sampleRate,
                                               const QVector<float>& parameterValues)
    : d(std::make_unique<Private>())
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    LilvWorldState& state = lilvState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.world == nullptr || uri.isEmpty() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return;
    const LilvPlugins* plugins = lilv_world_get_all_plugins(state.world);
    const QByteArray uriBytes = uri.toUtf8();
    LilvNode* uriNode = lilv_new_uri(state.world, uriBytes.constData());
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uriNode);
    lilv_node_free(uriNode);
    if (!describePlugin(plugin, d->descriptor))
        return;

    d->instance = lilv_plugin_instantiate(plugin, sampleRate, nullptr);
    if (d->instance == nullptr)
        return;
    d->controlInputs.reserve(static_cast<size_t>(d->descriptor.parameters.size()));
    for (qsizetype index = 0; index < d->descriptor.parameters.size(); ++index)
    {
        const Lv2EffectParameter& parameter = d->descriptor.parameters[index];
        float value = index < parameterValues.size() ? parameterValues[index] : parameter.defaultValue;
        if (!std::isfinite(value))
            value = parameter.defaultValue;
        value = std::clamp(value, parameter.minimum, parameter.maximum);
        if (parameter.toggled)
            value = value >= 0.5f ? 1.0f : 0.0f;
        else if (parameter.integer)
            value = std::round(value);
        d->controlInputs.push_back(value);
    }
    for (size_t index = 0; index < d->descriptor.audioInputs.size(); ++index)
    {
        float* location = d->descriptor.audioInputs.size() == 1
            ? &d->monoInput : index == 0 ? &d->inputLeft : &d->inputRight;
        lilv_instance_connect_port(d->instance, d->descriptor.audioInputs[index], location);
    }
    for (size_t index = 0; index < d->descriptor.audioOutputs.size(); ++index)
    {
        float* location = d->descriptor.audioOutputs.size() == 1
            ? &d->monoOutput : index == 0 ? &d->outputLeft : &d->outputRight;
        lilv_instance_connect_port(d->instance, d->descriptor.audioOutputs[index], location);
    }
    for (qsizetype index = 0; index < d->descriptor.parameters.size(); ++index)
    {
        lilv_instance_connect_port(d->instance, d->descriptor.parameters[index].portIndex,
                                   &d->controlInputs[static_cast<size_t>(index)]);
    }
    d->controlOutputs.assign(d->descriptor.controlOutputs.size(), 0.0f);
    for (size_t index = 0; index < d->descriptor.controlOutputs.size(); ++index)
        lilv_instance_connect_port(d->instance, d->descriptor.controlOutputs[index],
                                   &d->controlOutputs[index]);
    lilv_instance_activate(d->instance);
#else
    Q_UNUSED(uri)
    Q_UNUSED(sampleRate)
    Q_UNUSED(parameterValues)
#endif
}

Lv2AudioEffectInstance::~Lv2AudioEffectInstance()
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    if (d != nullptr && d->instance != nullptr)
    {
        lilv_instance_deactivate(d->instance);
        lilv_instance_free(d->instance);
    }
#endif
}

bool Lv2AudioEffectInstance::isValid() const
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    return d != nullptr && d->instance != nullptr;
#else
    return false;
#endif
}

qsizetype Lv2AudioEffectInstance::parameterCount() const
{
    return d == nullptr ? 0 : d->descriptor.parameters.size();
}

void Lv2AudioEffectInstance::setParameter(qsizetype index, float value)
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    if (d == nullptr || d->instance == nullptr || index < 0 ||
        index >= static_cast<qsizetype>(d->controlInputs.size()) ||
        index >= d->descriptor.parameters.size())
        return;
    const Lv2EffectParameter& parameter = d->descriptor.parameters[index];
    if (!std::isfinite(value))
        value = parameter.defaultValue;
    value = std::clamp(value, parameter.minimum, parameter.maximum);
    if (parameter.toggled)
        value = value >= 0.5f ? 1.0f : 0.0f;
    else if (parameter.integer)
        value = std::round(value);
    d->controlInputs[static_cast<size_t>(index)] = value;
#else
    Q_UNUSED(index)
    Q_UNUSED(value)
#endif
}

void Lv2AudioEffectInstance::process(float& left, float& right)
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    if (d == nullptr || d->instance == nullptr)
        return;
    d->inputLeft = left;
    d->inputRight = right;
    d->monoInput = (left + right) * 0.5f;
    d->outputLeft = 0.0f;
    d->outputRight = 0.0f;
    d->monoOutput = 0.0f;
    lilv_instance_run(d->instance, 1);
    if (d->descriptor.audioOutputs.size() == 1)
        left = right = d->monoOutput;
    else
    {
        left = d->outputLeft;
        right = d->outputRight;
    }
#else
    Q_UNUSED(left)
    Q_UNUSED(right)
#endif
}
