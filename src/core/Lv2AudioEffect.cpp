#include "Lv2AudioEffect.h"
#include "Project.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(AWESOME_STUDIO_HAS_LILV)
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#endif

namespace
{
#if defined(AWESOME_STUDIO_HAS_LILV)
struct LilvWorldState
{
    LilvWorldState()
    {
        uridMap.handle = this;
        uridMap.map = &LilvWorldState::mapUri;
        uridUnmap.handle = this;
        uridUnmap.unmap = &LilvWorldState::unmapUri;
        mapFeature.URI = LV2_URID__map;
        mapFeature.data = &uridMap;
        unmapFeature.URI = LV2_URID__unmap;
        unmapFeature.data = &uridUnmap;
        features[0] = &mapFeature;
        features[1] = &unmapFeature;
        features[2] = nullptr;

        world = lilv_world_new();
        if (world == nullptr)
            return;
        lilv_world_load_all(world);
        audioPort = lilv_new_uri(world, LV2_CORE__AudioPort);
        controlPort = lilv_new_uri(world, LV2_CORE__ControlPort);
        inputPort = lilv_new_uri(world, LV2_CORE__InputPort);
        outputPort = lilv_new_uri(world, LV2_CORE__OutputPort);
        designationProperty = lilv_new_uri(world, LV2_CORE__designation);
        latencyDesignation = lilv_new_uri(world, LV2_CORE__latency);
        integerProperty = lilv_new_uri(world, LV2_CORE__integer);
        reportsLatencyProperty = lilv_new_uri(world, LV2_CORE__reportsLatency);
        toggledProperty = lilv_new_uri(world, LV2_CORE__toggled);
    }

    ~LilvWorldState()
    {
        lilv_node_free(audioPort);
        lilv_node_free(controlPort);
        lilv_node_free(inputPort);
        lilv_node_free(outputPort);
        lilv_node_free(designationProperty);
        lilv_node_free(latencyDesignation);
        lilv_node_free(integerProperty);
        lilv_node_free(reportsLatencyProperty);
        lilv_node_free(toggledProperty);
        lilv_world_free(world);
    }

    LilvWorld* world = nullptr;
    std::mutex mutex;
    LilvNode* audioPort = nullptr;
    LilvNode* controlPort = nullptr;
    LilvNode* inputPort = nullptr;
    LilvNode* outputPort = nullptr;
    LilvNode* designationProperty = nullptr;
    LilvNode* latencyDesignation = nullptr;
    LilvNode* integerProperty = nullptr;
    LilvNode* reportsLatencyProperty = nullptr;
    LilvNode* toggledProperty = nullptr;
    LV2_URID_Map uridMap{};
    LV2_URID_Unmap uridUnmap{};
    LV2_Feature mapFeature{};
    LV2_Feature unmapFeature{};
    const LV2_Feature* features[3]{};
    std::mutex uridMutex;
    std::unordered_map<std::string, LV2_URID> uridByUri;
    std::deque<std::string> uriByUrid;

    static LV2_URID mapUri(LV2_URID_Map_Handle handle, const char* uri)
    {
        if (handle == nullptr || uri == nullptr || *uri == '\0')
            return 0;
        auto& state = *static_cast<LilvWorldState*>(handle);
        const std::lock_guard<std::mutex> lock(state.uridMutex);
        const auto found = state.uridByUri.find(uri);
        if (found != state.uridByUri.end())
            return found->second;
        if (state.uriByUrid.size() >= UINT32_MAX - 1)
            return 0;
        state.uriByUrid.emplace_back(uri);
        const LV2_URID id = static_cast<LV2_URID>(state.uriByUrid.size());
        state.uridByUri.emplace(state.uriByUrid.back(), id);
        return id;
    }

    static const char* unmapUri(LV2_URID_Unmap_Handle handle, LV2_URID urid)
    {
        if (handle == nullptr || urid == 0)
            return nullptr;
        auto& state = *static_cast<LilvWorldState*>(handle);
        const std::lock_guard<std::mutex> lock(state.uridMutex);
        const size_t index = static_cast<size_t>(urid - 1);
        return index < state.uriByUrid.size() ? state.uriByUrid[index].c_str() : nullptr;
    }
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
                LilvNodes* designationValues = state.designationProperty == nullptr
                    ? nullptr : lilv_port_get_value(plugin, port, state.designationProperty);
                const bool designatedLatency = designationValues != nullptr &&
                    state.latencyDesignation != nullptr &&
                    lilv_nodes_contains(designationValues, state.latencyDesignation);
                if (designationValues != nullptr)
                    lilv_nodes_free(designationValues);
                const bool reportsLatency = state.reportsLatencyProperty != nullptr &&
                    lilv_port_has_property(plugin, port, state.reportsLatencyProperty);
                if (designatedLatency || reportsLatency)
                {
                    if (descriptor.latencyOutputPort !=
                        std::numeric_limits<std::uint32_t>::max())
                        return false;
                    descriptor.latencyOutputPort = portIndex;
                }
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
    const LilvPlugin* plugin = nullptr;
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
    size_t latencyOutputIndex = std::numeric_limits<size_t>::max();
    std::atomic<qint64> reportedLatencyFrames{0};
    std::atomic<qint64> observedLatencyFrames{-1};
    std::atomic<qint64> pendingLatencyFrames{0};
    std::atomic<bool> latencyChangePending{false};
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
                                               const QVector<float>& parameterValues,
                                               const QString& pluginState)
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

    d->instance = lilv_plugin_instantiate(plugin, sampleRate, state.features);
    if (d->instance == nullptr)
        return;
    d->plugin = plugin;
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
    {
        lilv_instance_connect_port(d->instance, d->descriptor.controlOutputs[index],
                                   &d->controlOutputs[index]);
        if (d->descriptor.controlOutputs[index] == d->descriptor.latencyOutputPort)
            d->latencyOutputIndex = index;
    }

    if (!pluginState.isEmpty())
    {
        const QByteArray stateBytes = pluginState.toUtf8();
        LilvState* savedState = lilv_state_new_from_string(
            state.world, &state.uridMap, stateBytes.constData());
        if (savedState != nullptr)
        {
            const LilvNode* savedPlugin = lilv_state_get_plugin_uri(savedState);
            const char* savedPluginUri = savedPlugin == nullptr
                ? nullptr : lilv_node_as_string(savedPlugin);
            if (savedPluginUri != nullptr && uri == QString::fromUtf8(savedPluginUri))
            {
                lilv_state_restore(savedState, d->instance, nullptr, nullptr,
                                   LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                                   state.features);
            }
            lilv_state_free(savedState);
        }
    }

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

qint64 Lv2AudioEffectInstance::processingLatencyFrames() const
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    return d == nullptr ? 0 : d->reportedLatencyFrames.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

bool Lv2AudioEffectInstance::hasPendingLatencyChange() const
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    return d != nullptr && d->latencyChangePending.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool Lv2AudioEffectInstance::serviceLatencyChange()
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    if (d == nullptr || !d->latencyChangePending.exchange(false, std::memory_order_acq_rel))
        return false;
    const qint64 pendingLatency = std::max<qint64>(
        0, d->pendingLatencyFrames.load(std::memory_order_acquire));
    const qint64 previousLatency = d->reportedLatencyFrames.exchange(
        pendingLatency, std::memory_order_acq_rel);
    return previousLatency != pendingLatency;
#else
    return false;
#endif
}

qsizetype Lv2AudioEffectInstance::parameterCount() const
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    return d == nullptr ? 0 : d->descriptor.parameters.size();
#else
    return 0;
#endif
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

bool Lv2AudioEffectInstance::saveState(QString& pluginState) const
{
#if defined(AWESOME_STUDIO_HAS_LILV)
    if (d == nullptr || d->instance == nullptr || d->plugin == nullptr)
        return false;

    LilvWorldState& state = lilvState();
    const std::lock_guard<std::mutex> lock(state.mutex);
    LilvState* snapshot = lilv_state_new_from_instance(
        d->plugin, d->instance, &state.uridMap, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, state.features);
    if (snapshot == nullptr)
        return false;

    char* serialized = lilv_state_to_string(
        state.world, &state.uridMap, &state.uridUnmap, snapshot,
        "urn:awesome-studio:lv2-state", nullptr);
    lilv_state_free(snapshot);
    if (serialized == nullptr)
        return false;

    const size_t serializedSize = std::strlen(serialized);
    if (serializedSize == 0 || serializedSize >
        static_cast<size_t>(maximumPluginStateBytes))
    {
        lilv_free(serialized);
        return false;
    }
    pluginState = QString::fromUtf8(serialized, static_cast<qsizetype>(serializedSize));
    lilv_free(serialized);
    return !pluginState.isEmpty();
#else
    Q_UNUSED(pluginState)
    return false;
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
    if (d->latencyOutputIndex < d->controlOutputs.size())
    {
        const float reported = d->controlOutputs[d->latencyOutputIndex];
        if (std::isfinite(reported) && reported >= 0.0f)
        {
            const qint64 latency = static_cast<qint64>(std::llround(std::min<double>(
                reported, static_cast<double>(std::numeric_limits<std::uint32_t>::max()))));
            const qint64 previousLatency = d->observedLatencyFrames.exchange(
                latency, std::memory_order_relaxed);
            if (latency != previousLatency)
            {
                d->pendingLatencyFrames.store(latency, std::memory_order_relaxed);
                d->latencyChangePending.store(true, std::memory_order_release);
            }
        }
    }
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
