#include "ClapAudioEffect.h"

#include "Project.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(AWESOME_STUDIO_HAS_CLAP)
#include <clap/entry.h>
#include <clap/factory/plugin-factory.h>
#include <clap/plugin.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/log.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>

#include <dlfcn.h>
#endif

namespace
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
constexpr const char* clapStatePrefix = "CLAP_STATE_V1:";
constexpr uint32_t maximumClapParameters = 256;

struct ClapHostContext
{
    ClapHostContext()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Awesome Studio";
        host.vendor = "Awesome Studio";
        host.url = "https://github.com/Sipper1236/AwesomeStudio-2";
        host.version = "0.2.0";
        host.get_extension = &getExtension;
        host.request_restart = &requestRestart;
        host.request_process = &requestProcess;
        host.request_callback = &requestCallback;

        params.rescan = &rescanParameters;
        params.clear = &clearParameter;
        params.request_flush = &requestFlush;
        log.log = &writeLog;
        state.mark_dirty = &markStateDirty;
    }

    clap_host_t host{};
    clap_host_params_t params{};
    clap_host_log_t log{};
    clap_host_state_t state{};
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> restartRequested{false};
    std::atomic<bool> processRequested{false};
    std::atomic<bool> flushRequested{false};
    std::atomic<bool> stateDirty{false};

    static ClapHostContext* context(const clap_host_t* host)
    {
        return host == nullptr ? nullptr : static_cast<ClapHostContext*>(host->host_data);
    }

    static const void* CLAP_ABI getExtension(const clap_host_t* host, const char* id)
    {
        ClapHostContext* self = context(host);
        if (self == nullptr || id == nullptr)
            return nullptr;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &self->params;
        if (std::strcmp(id, CLAP_EXT_LOG) == 0)
            return &self->log;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &self->state;
        return nullptr;
    }

    static void CLAP_ABI requestRestart(const clap_host_t* host)
    {
        if (ClapHostContext* self = context(host))
            self->restartRequested.store(true, std::memory_order_release);
    }

    static void CLAP_ABI requestProcess(const clap_host_t* host)
    {
        if (ClapHostContext* self = context(host))
            self->processRequested.store(true, std::memory_order_release);
    }

    static void CLAP_ABI requestCallback(const clap_host_t* host)
    {
        if (ClapHostContext* self = context(host))
            self->callbackRequested.store(true, std::memory_order_release);
    }

    static void CLAP_ABI rescanParameters(const clap_host_t*, clap_param_rescan_flags) {}
    static void CLAP_ABI clearParameter(const clap_host_t*, clap_id, clap_param_clear_flags) {}

    static void CLAP_ABI requestFlush(const clap_host_t* host)
    {
        if (ClapHostContext* self = context(host))
            self->flushRequested.store(true, std::memory_order_release);
    }

    static void CLAP_ABI writeLog(const clap_host_t*, clap_log_severity, const char*) {}

    static void CLAP_ABI markStateDirty(const clap_host_t* host)
    {
        if (ClapHostContext* self = context(host))
            self->stateDirty.store(true, std::memory_order_release);
    }
};

struct ClapLibrary
{
    ~ClapLibrary()
    {
        if (initialized && entry != nullptr)
            entry->deinit();
        if (handle != nullptr)
            dlclose(handle);
    }

    QString path;
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* factory = nullptr;
    bool initialized = false;
};

std::mutex& libraryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<ClapLibrary>>& loadedLibraries()
{
    static std::vector<std::shared_ptr<ClapLibrary>> libraries;
    return libraries;
}

std::shared_ptr<ClapLibrary> loadLibrary(const QString& path)
{
    const std::lock_guard<std::mutex> lock(libraryMutex());
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    const QString stablePath = canonicalPath.isEmpty() ? QFileInfo(path).absoluteFilePath()
                                                        : canonicalPath;
    for (const auto& library : loadedLibraries())
        if (library->path == stablePath)
            return library;

    auto library = std::make_shared<ClapLibrary>();
    library->path = stablePath;
    const QByteArray encodedPath = QFile::encodeName(stablePath);
    library->handle = dlopen(encodedPath.constData(), RTLD_NOW | RTLD_LOCAL);
    if (library->handle == nullptr)
        return {};
    library->entry = static_cast<const clap_plugin_entry_t*>(dlsym(library->handle, "clap_entry"));
    if (library->entry == nullptr || !clap_version_is_compatible(library->entry->clap_version) ||
        library->entry->init == nullptr || library->entry->deinit == nullptr ||
        library->entry->get_factory == nullptr)
        return {};
    if (!library->entry->init(encodedPath.constData()))
        return {};
    library->initialized = true;
    library->factory = static_cast<const clap_plugin_factory_t*>(
        library->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (library->factory == nullptr || library->factory->get_plugin_count == nullptr ||
        library->factory->get_plugin_descriptor == nullptr ||
        library->factory->create_plugin == nullptr)
        return {};

    loadedLibraries().push_back(library);
    return library;
}

struct ClapCatalogEntry
{
    ClapEffectDescriptor descriptor;
    std::shared_ptr<ClapLibrary> library;
};

bool isAudioEffect(const clap_plugin_descriptor_t* plugin)
{
    if (plugin == nullptr || plugin->features == nullptr)
        return false;
    for (const char* const* feature = plugin->features; *feature != nullptr; ++feature)
        if (std::strcmp(*feature, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0)
            return true;
    return false;
}

bool inspectPlugin(const clap_plugin_t* plugin, const clap_plugin_descriptor_t* source,
                   const QString& path, ClapEffectDescriptor& result)
{
    if (plugin == nullptr || source == nullptr || !isAudioEffect(source))
        return false;
    const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (audioPorts == nullptr || audioPorts->count == nullptr || audioPorts->get == nullptr ||
        audioPorts->count(plugin, true) != 1 || audioPorts->count(plugin, false) != 1)
        return false;

    clap_audio_port_info_t inputInfo{};
    clap_audio_port_info_t outputInfo{};
    if (!audioPorts->get(plugin, 0, true, &inputInfo) ||
        !audioPorts->get(plugin, 0, false, &outputInfo) ||
        (inputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0 ||
        (outputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0 ||
        inputInfo.channel_count == 0 || inputInfo.channel_count > 2 ||
        outputInfo.channel_count == 0 || outputInfo.channel_count > 2)
        return false;

    const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
    if (notePorts != nullptr && notePorts->count != nullptr &&
        (notePorts->count(plugin, true) != 0 || notePorts->count(plugin, false) != 0))
        return false;

    result.id = QString::fromUtf8(source->id);
    result.name = QString::fromUtf8(source->name);
    result.vendor = source->vendor == nullptr ? QString() : QString::fromUtf8(source->vendor);
    result.path = path;
    result.inputChannels = inputInfo.channel_count;
    result.outputChannels = outputInfo.channel_count;
    result.hasState = plugin->get_extension(plugin, CLAP_EXT_STATE) != nullptr;
    const auto* params = static_cast<const clap_plugin_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    if (params == nullptr || params->count == nullptr || params->get_info == nullptr)
        return true;

    const uint32_t count = std::min(params->count(plugin), maximumClapParameters);
    result.parameters.reserve(static_cast<qsizetype>(count));
    for (uint32_t index = 0; index < count; ++index)
    {
        clap_param_info_t info{};
        if (!params->get_info(plugin, index, &info) ||
            info.id == CLAP_INVALID_ID || info.name[0] == '\0' ||
            !std::isfinite(info.min_value) || !std::isfinite(info.max_value) ||
            !std::isfinite(info.default_value) || info.max_value <= info.min_value ||
            info.default_value < info.min_value || info.default_value > info.max_value)
            continue;
        const bool integer = (info.flags & (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM)) != 0;
        const float minimum = static_cast<float>(info.min_value);
        const float maximum = static_cast<float>(info.max_value);
        const float defaultValue = static_cast<float>(info.default_value);
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(defaultValue))
            continue;
        result.parameters.push_back({info.id, QString::fromUtf8(info.name),
            minimum, maximum, defaultValue, integer,
            integer && info.min_value == 0.0 && info.max_value == 1.0,
            (info.flags & CLAP_PARAM_IS_HIDDEN) != 0,
            (info.flags & CLAP_PARAM_IS_READONLY) != 0});
    }
    return true;
}

std::vector<ClapCatalogEntry> buildCatalog()
{
    QStringList searchPaths{QDir::home().filePath(QStringLiteral(".clap")),
                            QStringLiteral("/usr/lib/clap"),
                            QStringLiteral("/usr/local/lib/clap")};
    const QString environmentPaths = qEnvironmentVariable("CLAP_PATH");
    for (const QString& path : environmentPaths.split(QDir::listSeparator(), Qt::SkipEmptyParts))
        searchPaths.push_back(path);

    QStringList pluginPaths;
    QSet<QString> uniquePaths;
    for (const QString& searchPath : std::as_const(searchPaths))
    {
        if (!QFileInfo(searchPath).isDir())
            continue;
        QDirIterator iterator(searchPath, {QStringLiteral("*.clap")},
                              QDir::Files | QDir::Readable,
                              QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (iterator.hasNext())
        {
            const QString path = iterator.next();
            const QString canonical = QFileInfo(path).canonicalFilePath();
            const QString stable = canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical;
            if (!uniquePaths.contains(stable))
            {
                uniquePaths.insert(stable);
                pluginPaths.push_back(stable);
            }
        }
    }

    std::vector<ClapCatalogEntry> result;
    for (const QString& path : std::as_const(pluginPaths))
    {
        const std::shared_ptr<ClapLibrary> library = loadLibrary(path);
        if (library == nullptr)
            continue;
        const uint32_t pluginCount = library->factory->get_plugin_count(library->factory);
        for (uint32_t index = 0; index < pluginCount; ++index)
        {
            const clap_plugin_descriptor_t* source =
                library->factory->get_plugin_descriptor(library->factory, index);
            if (source == nullptr || source->id == nullptr || source->name == nullptr ||
                !isAudioEffect(source))
                continue;

            ClapHostContext host;
            const clap_plugin_t* plugin = library->factory->create_plugin(
                library->factory, &host.host, source->id);
            if (plugin == nullptr)
                continue;
            const bool initialized = plugin->init != nullptr && plugin->init(plugin);
            ClapEffectDescriptor descriptor;
            const bool usable = initialized && inspectPlugin(plugin, source, path, descriptor);
            if (plugin->destroy != nullptr)
                plugin->destroy(plugin);
            if (usable)
                result.push_back({std::move(descriptor), library});
        }
    }
    std::sort(result.begin(), result.end(), [](const ClapCatalogEntry& left,
                                               const ClapCatalogEntry& right)
    {
        const int nameOrder = left.descriptor.name.compare(right.descriptor.name,
                                                            Qt::CaseInsensitive);
        return nameOrder == 0 ? left.descriptor.id < right.descriptor.id : nameOrder < 0;
    });
    return result;
}

const std::vector<ClapCatalogEntry>& catalog()
{
    static const std::vector<ClapCatalogEntry> entries = buildCatalog();
    return entries;
}

const ClapCatalogEntry* findCatalogEntry(const QString& id)
{
    const std::vector<ClapCatalogEntry>& entries = catalog();
    const auto found = std::find_if(entries.cbegin(), entries.cend(),
        [&id](const ClapCatalogEntry& entry) { return entry.descriptor.id == id; });
    return found == entries.cend() ? nullptr : &*found;
}

struct StateOutput
{
    QByteArray bytes;
    bool failed = false;

    static int64_t CLAP_ABI write(const clap_ostream_t* stream, const void* source,
                                  uint64_t size)
    {
        auto* self = stream == nullptr ? nullptr : static_cast<StateOutput*>(stream->ctx);
        if (self == nullptr || source == nullptr || size >
            static_cast<uint64_t>(maximumPluginStateBytes - self->bytes.size()))
        {
            if (self != nullptr)
                self->failed = true;
            return -1;
        }
        self->bytes.append(static_cast<const char*>(source), static_cast<qsizetype>(size));
        return static_cast<int64_t>(size);
    }
};

struct StateInput
{
    QByteArray bytes;
    qsizetype offset = 0;

    static int64_t CLAP_ABI read(const clap_istream_t* stream, void* destination,
                                 uint64_t size)
    {
        auto* self = stream == nullptr ? nullptr : static_cast<StateInput*>(stream->ctx);
        if (self == nullptr || destination == nullptr)
            return -1;
        const qsizetype remaining = self->bytes.size() - self->offset;
        const qsizetype count = std::min<qsizetype>(remaining,
            static_cast<qsizetype>(std::min<uint64_t>(size,
                static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()))));
        if (count <= 0)
            return 0;
        std::memcpy(destination, self->bytes.constData() + self->offset,
                    static_cast<size_t>(count));
        self->offset += count;
        return static_cast<int64_t>(count);
    }
};
#endif
}

struct ClapAudioEffectInstance::Private
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    std::shared_ptr<ClapLibrary> library;
    ClapHostContext hostContext;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    ClapEffectDescriptor descriptor;
    std::vector<float> controlValues;
    std::vector<float> sentValues;
    std::vector<clap_event_param_value_t> pendingEvents;
    clap_input_events_t inputEvents{};
    clap_output_events_t outputEvents{};
    float inputLeft = 0.0f;
    float inputRight = 0.0f;
    float monoInput = 0.0f;
    float outputLeft = 0.0f;
    float outputRight = 0.0f;
    float monoOutput = 0.0f;
    float* inputChannels[2]{};
    float* outputChannels[2]{};
    int64_t steadyTime = 0;
    std::thread::id processingThread;
    bool activated = false;
    bool processing = false;

    static Private* fromInputEvents(const clap_input_events_t* events)
    {
        return events == nullptr ? nullptr : static_cast<Private*>(events->ctx);
    }

    static uint32_t CLAP_ABI inputEventCount(const clap_input_events_t* events)
    {
        const Private* self = fromInputEvents(events);
        return self == nullptr ? 0 : static_cast<uint32_t>(self->pendingEvents.size());
    }

    static const clap_event_header_t* CLAP_ABI inputEventAt(
        const clap_input_events_t* events, uint32_t index)
    {
        const Private* self = fromInputEvents(events);
        return self == nullptr || index >= self->pendingEvents.size()
            ? nullptr : &self->pendingEvents[index].header;
    }

    static bool CLAP_ABI outputEventPush(const clap_output_events_t* events,
                                         const clap_event_header_t* event)
    {
        auto* self = events == nullptr ? nullptr : static_cast<Private*>(events->ctx);
        if (self == nullptr || event == nullptr || event->space_id != CLAP_CORE_EVENT_SPACE_ID ||
            event->type != CLAP_EVENT_PARAM_VALUE || event->size < sizeof(clap_event_param_value_t))
            return true;
        const auto* value = reinterpret_cast<const clap_event_param_value_t*>(event);
        for (qsizetype index = 0; index < self->descriptor.parameters.size(); ++index)
        {
            if (self->descriptor.parameters[static_cast<qsizetype>(index)].id == value->param_id &&
                std::isfinite(value->value))
            {
                const ClapEffectParameter& parameter =
                    self->descriptor.parameters[static_cast<qsizetype>(index)];
                float converted = std::clamp(static_cast<float>(value->value),
                                             parameter.minimum, parameter.maximum);
                if (parameter.toggled)
                    converted = converted >= 0.5f ? 1.0f : 0.0f;
                else if (parameter.integer)
                    converted = std::round(converted);
                self->controlValues[static_cast<size_t>(index)] = converted;
                self->sentValues[static_cast<size_t>(index)] = converted;
                break;
            }
        }
        return true;
    }

    bool loadState(const QString& text)
    {
        if (state == nullptr || state->load == nullptr || text.isEmpty() ||
            !text.startsWith(QLatin1String(clapStatePrefix)))
            return false;
        const QByteArray encoded = text.mid(static_cast<qsizetype>(std::strlen(clapStatePrefix)))
                                      .toLatin1();
        StateInput source{QByteArray::fromBase64(encoded), 0};
        if (source.bytes.isEmpty())
            return false;
        clap_istream_t stream{&source, &StateInput::read};
        return state->load(plugin, &stream);
    }

    void stopProcessing()
    {
        if (processing && plugin != nullptr && plugin->stop_processing != nullptr)
        {
            plugin->stop_processing(plugin);
            processing = false;
        }
    }

    ~Private()
    {
        if (plugin != nullptr)
        {
            if (processing && processingThread == std::this_thread::get_id())
                stopProcessing();
            if (activated && plugin->deactivate != nullptr)
                plugin->deactivate(plugin);
            if (plugin->destroy != nullptr)
                plugin->destroy(plugin);
        }
    }
#endif
};

QVector<ClapEffectDescriptor> ClapAudioEffectInstance::availableEffects()
{
    QVector<ClapEffectDescriptor> result;
#if defined(AWESOME_STUDIO_HAS_CLAP)
    const std::vector<ClapCatalogEntry>& entries = catalog();
    result.reserve(static_cast<qsizetype>(entries.size()));
    for (const ClapCatalogEntry& entry : entries)
        result.push_back(entry.descriptor);
#endif
    return result;
}

bool ClapAudioEffectInstance::describe(const QString& id, ClapEffectDescriptor& descriptor)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    const ClapCatalogEntry* entry = findCatalogEntry(id);
    if (entry == nullptr)
        return false;
    descriptor = entry->descriptor;
    return true;
#else
    Q_UNUSED(id)
    Q_UNUSED(descriptor)
    return false;
#endif
}

ClapAudioEffectInstance::ClapAudioEffectInstance(const QString& id, double sampleRate,
                                                 const QVector<float>& parameterValues,
                                                 const QString& pluginState)
    : d(std::make_unique<Private>())
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return;
    const ClapCatalogEntry* entry = findCatalogEntry(id);
    if (entry == nullptr)
        return;
    d->library = entry->library;
    d->descriptor = entry->descriptor;
    const QByteArray pluginId = d->descriptor.id.toUtf8();
    d->plugin = d->library->factory->create_plugin(d->library->factory,
                                                    &d->hostContext.host,
                                                    pluginId.constData());
    if (d->plugin == nullptr)
        return;
    if (d->plugin->init == nullptr || !d->plugin->init(d->plugin))
    {
        if (d->plugin->destroy != nullptr)
            d->plugin->destroy(d->plugin);
        d->plugin = nullptr;
        return;
    }

    d->params = static_cast<const clap_plugin_params_t*>(
        d->plugin->get_extension(d->plugin, CLAP_EXT_PARAMS));
    d->state = static_cast<const clap_plugin_state_t*>(
        d->plugin->get_extension(d->plugin, CLAP_EXT_STATE));
    if (d->state != nullptr)
        d->loadState(pluginState);

    d->controlValues.resize(static_cast<size_t>(d->descriptor.parameters.size()));
    d->sentValues.assign(d->controlValues.size(), std::numeric_limits<float>::quiet_NaN());
    d->pendingEvents.reserve(d->controlValues.size());
    for (qsizetype index = 0; index < d->descriptor.parameters.size(); ++index)
    {
        const ClapEffectParameter& parameter = d->descriptor.parameters[index];
        double currentValue = parameter.defaultValue;
        if (d->params != nullptr && d->params->get_value != nullptr)
            d->params->get_value(d->plugin, parameter.id, &currentValue);
        float value = index < parameterValues.size()
            ? parameterValues[index] : static_cast<float>(currentValue);
        if (!std::isfinite(value))
            value = parameter.defaultValue;
        value = std::clamp(value, parameter.minimum, parameter.maximum);
        if (parameter.toggled)
            value = value >= 0.5f ? 1.0f : 0.0f;
        else if (parameter.integer)
            value = std::round(value);
        d->controlValues[static_cast<size_t>(index)] = value;
    }

    if (d->descriptor.inputChannels == 0 || d->descriptor.outputChannels == 0)
        return;

    d->inputChannels[0] = d->descriptor.inputChannels == 1 ? &d->monoInput : &d->inputLeft;
    d->inputChannels[1] = &d->inputRight;
    d->outputChannels[0] = d->descriptor.outputChannels == 1 ? &d->monoOutput : &d->outputLeft;
    d->outputChannels[1] = &d->outputRight;

    if (d->plugin->activate == nullptr || !d->plugin->activate(d->plugin, sampleRate, 1, 1))
        return;
    d->activated = true;
    d->inputEvents.ctx = d.get();
    d->inputEvents.size = &Private::inputEventCount;
    d->inputEvents.get = &Private::inputEventAt;
    d->outputEvents.ctx = d.get();
    d->outputEvents.try_push = &Private::outputEventPush;
#else
    Q_UNUSED(id)
    Q_UNUSED(sampleRate)
    Q_UNUSED(parameterValues)
    Q_UNUSED(pluginState)
#endif
}

ClapAudioEffectInstance::~ClapAudioEffectInstance() = default;

bool ClapAudioEffectInstance::isValid() const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    return d != nullptr && d->plugin != nullptr && d->activated;
#else
    return false;
#endif
}

qsizetype ClapAudioEffectInstance::parameterCount() const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    return d == nullptr ? 0 : d->descriptor.parameters.size();
#else
    return 0;
#endif
}

void ClapAudioEffectInstance::setParameter(qsizetype index, float value)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || !isValid() || index < 0 || index >= d->descriptor.parameters.size())
        return;
    const ClapEffectParameter& parameter = d->descriptor.parameters[index];
    if (!std::isfinite(value))
        value = parameter.defaultValue;
    value = std::clamp(value, parameter.minimum, parameter.maximum);
    if (parameter.toggled)
        value = value >= 0.5f ? 1.0f : 0.0f;
    else if (parameter.integer)
        value = std::round(value);
    d->controlValues[static_cast<size_t>(index)] = value;
#else
    Q_UNUSED(index)
    Q_UNUSED(value)
#endif
}

bool ClapAudioEffectInstance::saveState(QString& pluginState) const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (!isValid() || d->state == nullptr || d->state->save == nullptr)
        return false;
    StateOutput destination;
    clap_ostream_t stream{&destination, &StateOutput::write};
    if (!d->state->save(d->plugin, &stream) || destination.failed || destination.bytes.isEmpty())
        return false;
    const QByteArray encoded = destination.bytes.toBase64();
    const QByteArray prefix(clapStatePrefix);
    if (encoded.size() + prefix.size() > maximumPluginStateBytes)
        return false;
    pluginState = QString::fromLatin1(prefix + encoded);
    return true;
#else
    Q_UNUSED(pluginState)
    return false;
#endif
}

void ClapAudioEffectInstance::serviceMainThread()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (isValid() && d->hostContext.callbackRequested.exchange(false, std::memory_order_acq_rel) &&
        d->plugin->on_main_thread != nullptr)
        d->plugin->on_main_thread(d->plugin);
#endif
}

void ClapAudioEffectInstance::stopProcessing()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d != nullptr && d->processing &&
        d->processingThread == std::this_thread::get_id())
        d->stopProcessing();
#endif
}

bool ClapAudioEffectInstance::process(float& left, float& right)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (!isValid())
        return false;
    if (d->plugin->start_processing == nullptr || d->plugin->stop_processing == nullptr ||
        d->plugin->process == nullptr)
        return false;
    if (!d->processing)
    {
        if (d->plugin->start_processing == nullptr || !d->plugin->start_processing(d->plugin))
            return false;
        d->processing = true;
        d->processingThread = std::this_thread::get_id();
    }

    d->pendingEvents.clear();
    for (size_t index = 0; index < d->controlValues.size(); ++index)
    {
        const float value = d->controlValues[index];
        const ClapEffectParameter& parameter =
            d->descriptor.parameters[static_cast<qsizetype>(index)];
        if (parameter.readOnly)
        {
            d->sentValues[index] = value;
            continue;
        }
        if (!std::isfinite(d->sentValues[index]) ||
            std::abs(d->sentValues[index] - value) > 1.0e-7f)
        {
            clap_event_param_value_t event{};
            event.header.size = sizeof(event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = CLAP_EVENT_DONT_RECORD;
            event.param_id = parameter.id;
            event.cookie = nullptr;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = value;
            d->pendingEvents.push_back(event);
            d->sentValues[index] = value;
        }
    }

    if (d->hostContext.flushRequested.exchange(false, std::memory_order_acq_rel) &&
        d->params != nullptr && d->params->flush != nullptr)
        d->params->flush(d->plugin, &d->inputEvents, &d->outputEvents);

    d->inputLeft = left;
    d->inputRight = right;
    d->monoInput = (left + right) * 0.5f;
    d->outputLeft = d->outputRight = d->monoOutput = 0.0f;
    clap_audio_buffer_t inputBuffer{};
    inputBuffer.data32 = d->inputChannels;
    inputBuffer.channel_count = d->descriptor.inputChannels;
    clap_audio_buffer_t outputBuffer{};
    outputBuffer.data32 = d->outputChannels;
    outputBuffer.channel_count = d->descriptor.outputChannels;

    clap_process_t process{};
    process.steady_time = d->steadyTime++;
    process.frames_count = 1;
    process.audio_inputs = &inputBuffer;
    process.audio_outputs = &outputBuffer;
    process.audio_inputs_count = 1;
    process.audio_outputs_count = 1;
    process.in_events = &d->inputEvents;
    process.out_events = &d->outputEvents;
    const clap_process_status status = d->plugin->process(d->plugin, &process);
    if (status == CLAP_PROCESS_ERROR)
        return false;
    if (outputBuffer.channel_count == 1)
        left = right = d->monoOutput;
    else
    {
        left = d->outputLeft;
        right = d->outputRight;
    }
    return std::isfinite(left) && std::isfinite(right);
#else
    Q_UNUSED(left)
    Q_UNUSED(right)
    return false;
#endif
}
