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
#include <array>
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
#include <clap/events.h>
#include <clap/factory/plugin-factory.h>
#include <clap/plugin.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/latency.h>
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
constexpr uint32_t clapBlockFrames = 64;

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
        latency.changed = &latencyChanged;
    }

    clap_host_t host{};
    clap_host_params_t params{};
    clap_host_log_t log{};
    clap_host_state_t state{};
    clap_host_latency_t latency{};
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
        if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
            return &self->latency;
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

    static void CLAP_ABI latencyChanged(const clap_host_t*) {}
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
    ClapPluginDescriptor descriptor;
    ClapPluginKind kind = ClapPluginKind::AudioEffect;
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

bool isInstrument(const clap_plugin_descriptor_t* plugin)
{
    if (plugin == nullptr || plugin->features == nullptr)
        return false;
    for (const char* const* feature = plugin->features; *feature != nullptr; ++feature)
        if (std::strcmp(*feature, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0)
            return true;
    return false;
}

bool inspectPlugin(const clap_plugin_t* plugin, const clap_plugin_descriptor_t* source,
                   const QString& path, ClapPluginKind kind, ClapPluginDescriptor& result)
{
    if (plugin == nullptr || source == nullptr ||
        (kind == ClapPluginKind::AudioEffect ? !isAudioEffect(source) : !isInstrument(source)))
        return false;
    const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (audioPorts == nullptr || audioPorts->count == nullptr || audioPorts->get == nullptr)
        return false;

    const uint32_t inputCount = audioPorts->count(plugin, true);
    const uint32_t outputCount = audioPorts->count(plugin, false);
    if ((kind == ClapPluginKind::AudioEffect && (inputCount != 1 || outputCount != 1)) ||
        (kind == ClapPluginKind::Instrument && (inputCount != 0 || outputCount != 1)))
        return false;

    clap_audio_port_info_t inputInfo{};
    clap_audio_port_info_t outputInfo{};
    if ((kind == ClapPluginKind::AudioEffect &&
         (!audioPorts->get(plugin, 0, true, &inputInfo) ||
          (inputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0 ||
          inputInfo.channel_count == 0 || inputInfo.channel_count > 2)) ||
        !audioPorts->get(plugin, 0, false, &outputInfo) ||
        (outputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0 ||
        outputInfo.channel_count == 0 || outputInfo.channel_count > 2)
        return false;

    const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
    clap_note_port_info_t noteInput{};
    if (kind == ClapPluginKind::AudioEffect)
    {
        if (notePorts != nullptr && notePorts->count != nullptr &&
            (notePorts->count(plugin, true) != 0 || notePorts->count(plugin, false) != 0))
            return false;
    }
    else
    {
        if (notePorts == nullptr || notePorts->count == nullptr || notePorts->get == nullptr ||
            notePorts->count(plugin, true) != 1 || notePorts->count(plugin, false) != 0 ||
            !notePorts->get(plugin, 0, true, &noteInput) ||
            (noteInput.supported_dialects & CLAP_NOTE_DIALECT_CLAP) == 0)
            return false;
    }

    result.id = QString::fromUtf8(source->id);
    result.name = QString::fromUtf8(source->name);
    result.vendor = source->vendor == nullptr ? QString() : QString::fromUtf8(source->vendor);
    result.path = path;
    result.inputChannels = kind == ClapPluginKind::AudioEffect ? inputInfo.channel_count : 0;
    result.outputChannels = outputInfo.channel_count;
    result.supportsMidiDialect = kind == ClapPluginKind::Instrument &&
        notePorts != nullptr && notePorts->count(plugin, true) == 1 &&
        notePorts->get(plugin, 0, true, &noteInput) &&
        (noteInput.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
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

std::vector<ClapCatalogEntry> buildCatalog(ClapPluginKind kind)
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
                (kind == ClapPluginKind::AudioEffect ? !isAudioEffect(source) : !isInstrument(source)))
                continue;

            ClapHostContext host;
            const clap_plugin_t* plugin = library->factory->create_plugin(
                library->factory, &host.host, source->id);
            if (plugin == nullptr)
                continue;
            const bool initialized = plugin->init != nullptr && plugin->init(plugin);
            ClapPluginDescriptor descriptor;
            const bool usable = initialized && inspectPlugin(plugin, source, path, kind, descriptor);
            if (plugin->destroy != nullptr)
                plugin->destroy(plugin);
            if (usable)
                result.push_back({std::move(descriptor), kind, library});
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

const std::vector<ClapCatalogEntry>& catalog(ClapPluginKind kind)
{
    if (kind == ClapPluginKind::Instrument)
    {
        static const std::vector<ClapCatalogEntry> instruments =
            buildCatalog(ClapPluginKind::Instrument);
        return instruments;
    }
    static const std::vector<ClapCatalogEntry> effects = buildCatalog(ClapPluginKind::AudioEffect);
    return effects;
}

const ClapCatalogEntry* findCatalogEntry(const QString& id, ClapPluginKind kind)
{
    const std::vector<ClapCatalogEntry>& entries = catalog(kind);
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

struct ClapAudioPluginInstance::Private
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    struct PendingEvent
    {
        enum class Kind { Note, Midi, Expression, Parameter } kind = Kind::Note;
        union Payload
        {
            clap_event_note_t note;
            clap_event_midi_t midi;
            clap_event_note_expression_t expression;
            clap_event_param_value_t parameter;
            Payload() : note{} {}
        } payload;

        const clap_event_header_t* header() const
        {
            switch (kind)
            {
            case Kind::Note: return &payload.note.header;
            case Kind::Midi: return &payload.midi.header;
            case Kind::Expression: return &payload.expression.header;
            case Kind::Parameter: return &payload.parameter.header;
            }
            return nullptr;
        }
    };

    std::shared_ptr<ClapLibrary> library;
    ClapHostContext hostContext;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_latency_t* latency = nullptr;
    ClapPluginKind kind = ClapPluginKind::AudioEffect;
    ClapPluginDescriptor descriptor;
    std::vector<float> controlValues;
    std::vector<float> sentValues;
    std::vector<PendingEvent> pendingEvents;
    std::vector<clap_event_param_value_t> pendingParameterEvents;
    size_t pendingInstrumentEventCount = 0;
    clap_input_events_t inputEvents{};
    clap_input_events_t parameterEvents{};
    clap_output_events_t outputEvents{};
    std::array<float, clapBlockFrames> inputLeft{};
    std::array<float, clapBlockFrames> inputRight{};
    std::array<float, clapBlockFrames> monoInput{};
    std::array<float, clapBlockFrames> outputLeft{};
    std::array<float, clapBlockFrames> outputRight{};
    std::array<float, clapBlockFrames> monoOutput{};
    std::array<float, clapBlockFrames> dryLeft{};
    std::array<float, clapBlockFrames> dryRight{};
    float* inputChannels[2]{};
    float* outputChannels[2]{};
    uint32_t bufferedFrames = 0;
    uint32_t outputRead = 0;
    bool outputReady = false;
    int64_t steadyTime = 0;
    uint32_t reportedLatencyFrames = 0;
    double sampleRate = 44100.0;
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
            ? nullptr : self->pendingEvents[index].header();
    }

    static uint32_t CLAP_ABI parameterEventCount(const clap_input_events_t* events)
    {
        const Private* self = fromInputEvents(events);
        return self == nullptr ? 0 : static_cast<uint32_t>(self->pendingParameterEvents.size());
    }

    static const clap_event_header_t* CLAP_ABI parameterEventAt(
        const clap_input_events_t* events, uint32_t index)
    {
        const Private* self = fromInputEvents(events);
        return self == nullptr || index >= self->pendingParameterEvents.size()
            ? nullptr : &self->pendingParameterEvents[index].header;
    }

    void queueNoteEvent(uint16_t type, int32_t noteId, int channel, int key, float velocity)
    {
        if (kind != ClapPluginKind::Instrument ||
            pendingInstrumentEventCount >= 512 ||
            pendingEvents.size() >= pendingEvents.capacity())
            return;
        ++pendingInstrumentEventCount;
        PendingEvent& pending = pendingEvents.emplace_back();
        pending.kind = PendingEvent::Kind::Note;
        pending.payload.note = {};
        pending.payload.note.header.size = sizeof(clap_event_note_t);
        pending.payload.note.header.time = bufferedFrames;
        pending.payload.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        pending.payload.note.header.type = type;
        pending.payload.note.header.flags = CLAP_EVENT_IS_LIVE;
        pending.payload.note.note_id = noteId;
        pending.payload.note.port_index = 0;
        pending.payload.note.channel = static_cast<int16_t>(channel);
        pending.payload.note.key = static_cast<int16_t>(key);
        pending.payload.note.velocity = velocity;
    }

    void queueMidiEvent(uint16_t portIndex, const std::array<uint8_t, 3>& data)
    {
        if (kind != ClapPluginKind::Instrument ||
            pendingInstrumentEventCount >= 512 ||
            pendingEvents.size() >= pendingEvents.capacity())
            return;
        ++pendingInstrumentEventCount;
        PendingEvent& pending = pendingEvents.emplace_back();
        pending.kind = PendingEvent::Kind::Midi;
        pending.payload.midi = {};
        pending.payload.midi.header.size = sizeof(clap_event_midi_t);
        pending.payload.midi.header.time = bufferedFrames;
        pending.payload.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        pending.payload.midi.header.type = CLAP_EVENT_MIDI;
        pending.payload.midi.header.flags = CLAP_EVENT_IS_LIVE;
        pending.payload.midi.port_index = portIndex;
        std::copy(data.cbegin(), data.cend(), pending.payload.midi.data);
    }

    void queueNotePressure(std::int32_t noteId, int channel, int key, float value)
    {
        if (kind != ClapPluginKind::Instrument ||
            pendingInstrumentEventCount >= 512 ||
            pendingEvents.size() >= pendingEvents.capacity())
            return;
        ++pendingInstrumentEventCount;
        PendingEvent& pending = pendingEvents.emplace_back();
        pending.kind = PendingEvent::Kind::Expression;
        pending.payload.expression = {};
        pending.payload.expression.header.size = sizeof(clap_event_note_expression_t);
        pending.payload.expression.header.time = bufferedFrames;
        pending.payload.expression.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        pending.payload.expression.header.type = CLAP_EVENT_NOTE_EXPRESSION;
        pending.payload.expression.header.flags = CLAP_EVENT_IS_LIVE;
        pending.payload.expression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
        pending.payload.expression.note_id = noteId;
        pending.payload.expression.port_index = 0;
        pending.payload.expression.channel = static_cast<int16_t>(channel);
        pending.payload.expression.key = static_cast<int16_t>(key);
        pending.payload.expression.value = std::clamp(static_cast<double>(value), 0.0, 1.0);
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
                const ClapPluginParameter& parameter =
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

QVector<ClapPluginDescriptor> ClapAudioPluginInstance::availablePlugins(ClapPluginKind kind)
{
    QVector<ClapPluginDescriptor> result;
#if defined(AWESOME_STUDIO_HAS_CLAP)
    const std::vector<ClapCatalogEntry>& entries = catalog(kind);
    result.reserve(static_cast<qsizetype>(entries.size()));
    for (const ClapCatalogEntry& entry : entries)
        result.push_back(entry.descriptor);
#endif
    return result;
}

bool ClapAudioPluginInstance::describe(const QString& id, ClapPluginKind kind,
                                       ClapPluginDescriptor& descriptor)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    const ClapCatalogEntry* entry = findCatalogEntry(id, kind);
    if (entry == nullptr)
        return false;
    descriptor = entry->descriptor;
    return true;
#else
    Q_UNUSED(id)
    Q_UNUSED(kind)
    Q_UNUSED(descriptor)
    return false;
#endif
}

ClapAudioPluginInstance::ClapAudioPluginInstance(const QString& id, ClapPluginKind kind,
                                                 double sampleRate,
                                                 const QVector<float>& parameterValues,
                                                 const QString& pluginState)
    : d(std::make_unique<Private>())
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    d->kind = kind;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return;
    d->sampleRate = sampleRate;
    const ClapCatalogEntry* entry = findCatalogEntry(id, kind);
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
    d->latency = static_cast<const clap_plugin_latency_t*>(
        d->plugin->get_extension(d->plugin, CLAP_EXT_LATENCY));
    if (d->state != nullptr)
        d->loadState(pluginState);

    d->controlValues.resize(static_cast<size_t>(d->descriptor.parameters.size()));
    d->sentValues.assign(d->controlValues.size(), std::numeric_limits<float>::quiet_NaN());
    d->pendingEvents.reserve(d->controlValues.size() * clapBlockFrames + 512);
    d->pendingParameterEvents.reserve(d->controlValues.size());
    for (qsizetype index = 0; index < d->descriptor.parameters.size(); ++index)
    {
        const ClapPluginParameter& parameter = d->descriptor.parameters[index];
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

    if ((kind == ClapPluginKind::AudioEffect && d->descriptor.inputChannels == 0) ||
        d->descriptor.outputChannels == 0)
        return;

    d->inputChannels[0] = d->descriptor.inputChannels == 1
        ? d->monoInput.data() : d->inputLeft.data();
    d->inputChannels[1] = d->inputRight.data();
    d->outputChannels[0] = d->descriptor.outputChannels == 1
        ? d->monoOutput.data() : d->outputLeft.data();
    d->outputChannels[1] = d->outputRight.data();

    if (d->plugin->activate == nullptr ||
        !d->plugin->activate(d->plugin, sampleRate, 1, clapBlockFrames))
        return;
    d->activated = true;
    if (d->latency != nullptr && d->latency->get != nullptr)
        d->reportedLatencyFrames = d->latency->get(d->plugin);
    d->inputEvents.ctx = d.get();
    d->inputEvents.size = &Private::inputEventCount;
    d->inputEvents.get = &Private::inputEventAt;
    d->parameterEvents.ctx = d.get();
    d->parameterEvents.size = &Private::parameterEventCount;
    d->parameterEvents.get = &Private::parameterEventAt;
    d->outputEvents.ctx = d.get();
    d->outputEvents.try_push = &Private::outputEventPush;
#else
    Q_UNUSED(id)
    Q_UNUSED(kind)
    Q_UNUSED(sampleRate)
    Q_UNUSED(parameterValues)
    Q_UNUSED(pluginState)
#endif
}

ClapAudioPluginInstance::~ClapAudioPluginInstance() = default;

bool ClapAudioPluginInstance::isValid() const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    return d != nullptr && d->plugin != nullptr && d->activated;
#else
    return false;
#endif
}

bool ClapAudioPluginInstance::hasPendingRestart() const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    return d != nullptr &&
        d->hostContext.restartRequested.load(std::memory_order_acquire);
#else
    return false;
#endif
}

bool ClapAudioPluginInstance::serviceRestart()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || d->plugin == nullptr || !d->hostContext.restartRequested.exchange(
            false, std::memory_order_acq_rel))
        return false;
    if (d->processing)
    {
        d->hostContext.restartRequested.store(true, std::memory_order_release);
        return false;
    }

    const qint64 previousLatency = d->activated
        ? static_cast<qint64>(clapBlockFrames) + d->reportedLatencyFrames : 0;
    if (d->activated)
    {
        if (d->plugin->deactivate == nullptr)
        {
            d->hostContext.restartRequested.store(true, std::memory_order_release);
            return false;
        }
        d->plugin->deactivate(d->plugin);
        d->activated = false;
    }
    if (d->plugin->activate == nullptr ||
        !d->plugin->activate(d->plugin, d->sampleRate, 1, clapBlockFrames))
    {
        d->hostContext.restartRequested.store(true, std::memory_order_release);
        return previousLatency != 0;
    }
    d->activated = true;
    d->reportedLatencyFrames = d->latency != nullptr && d->latency->get != nullptr
        ? d->latency->get(d->plugin) : 0;
    d->bufferedFrames = 0;
    d->outputRead = 0;
    d->outputReady = false;
    d->pendingEvents.clear();
    d->pendingParameterEvents.clear();
    d->pendingInstrumentEventCount = 0;
    d->steadyTime = 0;
    d->inputLeft.fill(0.0f);
    d->inputRight.fill(0.0f);
    d->monoInput.fill(0.0f);
    d->outputLeft.fill(0.0f);
    d->outputRight.fill(0.0f);
    d->monoOutput.fill(0.0f);
    d->dryLeft.fill(0.0f);
    d->dryRight.fill(0.0f);
    std::fill(d->sentValues.begin(), d->sentValues.end(),
              std::numeric_limits<float>::quiet_NaN());
    return true;
#else
    return false;
#endif
}

qint64 ClapAudioPluginInstance::processingLatencyFrames() const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    return isValid() ? static_cast<qint64>(clapBlockFrames) + d->reportedLatencyFrames : 0;
#else
    return 0;
#endif
}

qsizetype ClapAudioPluginInstance::parameterCount() const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    return d == nullptr ? 0 : d->descriptor.parameters.size();
#else
    return 0;
#endif
}

float ClapAudioPluginInstance::parameterValue(qsizetype index) const
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || index < 0 || index >= static_cast<qsizetype>(d->controlValues.size()))
        return 0.0f;
    return d->controlValues[static_cast<size_t>(index)];
#else
    Q_UNUSED(index)
    return 0.0f;
#endif
}

void ClapAudioPluginInstance::setParameter(qsizetype index, float value)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || !isValid() || index < 0 || index >= d->descriptor.parameters.size())
        return;
    const ClapPluginParameter& parameter = d->descriptor.parameters[index];
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

void ClapAudioPluginInstance::noteOn(std::int32_t noteId, int channel, int key, float velocity)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || !isValid() || !std::isfinite(velocity))
        return;
    d->queueNoteEvent(CLAP_EVENT_NOTE_ON, noteId, std::clamp(channel, 0, 15),
                      std::clamp(key, 0, 127), std::clamp(velocity, 0.0f, 1.0f));
#else
    Q_UNUSED(noteId)
    Q_UNUSED(channel)
    Q_UNUSED(key)
    Q_UNUSED(velocity)
#endif
}

void ClapAudioPluginInstance::noteOff(std::int32_t noteId, int channel, int key, float velocity)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || !isValid() || !std::isfinite(velocity))
        return;
    d->queueNoteEvent(CLAP_EVENT_NOTE_OFF, noteId, std::clamp(channel, 0, 15),
                      std::clamp(key, 0, 127), std::clamp(velocity, 0.0f, 1.0f));
#else
    Q_UNUSED(noteId)
    Q_UNUSED(channel)
    Q_UNUSED(key)
    Q_UNUSED(velocity)
#endif
}

void ClapAudioPluginInstance::allNotesOff()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d != nullptr && isValid())
        d->queueNoteEvent(CLAP_EVENT_NOTE_CHOKE, -1, -1, -1, 0.0f);
#endif
}

void ClapAudioPluginInstance::pitchBend(int channel, int value)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || !isValid() || !d->descriptor.supportsMidiDialect)
        return;
    const int safeChannel = std::clamp(channel, 0, 15);
    const int safeValue = std::clamp(value, 0, 16383);
    d->queueMidiEvent(0, {static_cast<uint8_t>(0xE0 | safeChannel),
                          static_cast<uint8_t>(safeValue & 0x7F),
                          static_cast<uint8_t>((safeValue >> 7) & 0x7F)});
#else
    Q_UNUSED(channel)
    Q_UNUSED(value)
#endif
}

void ClapAudioPluginInstance::notePressure(std::int32_t noteId, int channel, int key, float value)
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr || !isValid() || !std::isfinite(value))
        return;
    d->queueNotePressure(noteId, std::clamp(channel, 0, 15),
                         std::clamp(key, 0, 127), value);
#else
    Q_UNUSED(noteId)
    Q_UNUSED(channel)
    Q_UNUSED(key)
    Q_UNUSED(value)
#endif
}

bool ClapAudioPluginInstance::saveState(QString& pluginState) const
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

void ClapAudioPluginInstance::serviceMainThread()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (isValid() && d->hostContext.callbackRequested.exchange(false, std::memory_order_acq_rel) &&
        d->plugin->on_main_thread != nullptr)
        d->plugin->on_main_thread(d->plugin);
#endif
}

void ClapAudioPluginInstance::stopProcessing()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d != nullptr && d->processing &&
        d->processingThread == std::this_thread::get_id())
        d->stopProcessing();
#endif
}

void ClapAudioPluginInstance::resetProcessingState()
{
#if defined(AWESOME_STUDIO_HAS_CLAP)
    if (d == nullptr)
        return;
    d->bufferedFrames = 0;
    d->outputRead = 0;
    d->outputReady = false;
    d->pendingEvents.clear();
    d->pendingParameterEvents.clear();
    d->pendingInstrumentEventCount = 0;
    d->steadyTime = 0;
    d->inputLeft.fill(0.0f);
    d->inputRight.fill(0.0f);
    d->monoInput.fill(0.0f);
    d->outputLeft.fill(0.0f);
    d->outputRight.fill(0.0f);
    d->monoOutput.fill(0.0f);
    d->dryLeft.fill(0.0f);
    d->dryRight.fill(0.0f);
    if (isValid() && d->plugin->reset != nullptr)
        d->plugin->reset(d->plugin);
#endif
}

bool ClapAudioPluginInstance::process(float& left, float& right)
{
    float alignedDryLeft = 0.0f;
    float alignedDryRight = 0.0f;
    return process(left, right, alignedDryLeft, alignedDryRight);
}

bool ClapAudioPluginInstance::process(float& left, float& right,
                                      float& alignedDryLeft, float& alignedDryRight)
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

    d->pendingParameterEvents.clear();
    for (size_t index = 0; index < d->controlValues.size(); ++index)
    {
        const float value = d->controlValues[index];
        const ClapPluginParameter& parameter =
            d->descriptor.parameters[static_cast<qsizetype>(index)];
        if (parameter.readOnly)
        {
            d->sentValues[index] = value;
            continue;
        }
        if (!std::isfinite(d->sentValues[index]) ||
            std::abs(d->sentValues[index] - value) > 1.0e-7f)
        {
            Private::PendingEvent pending;
            pending.kind = Private::PendingEvent::Kind::Parameter;
            pending.payload.parameter = {};
            pending.payload.parameter.header.size = sizeof(clap_event_param_value_t);
            pending.payload.parameter.header.time = d->bufferedFrames;
            pending.payload.parameter.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            pending.payload.parameter.header.type = CLAP_EVENT_PARAM_VALUE;
            pending.payload.parameter.header.flags = CLAP_EVENT_DONT_RECORD;
            pending.payload.parameter.param_id = parameter.id;
            pending.payload.parameter.cookie = nullptr;
            pending.payload.parameter.note_id = -1;
            pending.payload.parameter.port_index = -1;
            pending.payload.parameter.channel = -1;
            pending.payload.parameter.key = -1;
            pending.payload.parameter.value = value;
            if (d->pendingEvents.size() < d->pendingEvents.capacity())
                d->pendingEvents.push_back(pending);

            clap_event_param_value_t flushEvent = pending.payload.parameter;
            flushEvent.header.time = 0;
            d->pendingParameterEvents.push_back(flushEvent);
            d->sentValues[index] = value;
        }
    }

    if (d->hostContext.flushRequested.exchange(false, std::memory_order_acq_rel) &&
        d->params != nullptr && d->params->flush != nullptr)
        d->params->flush(d->plugin, &d->parameterEvents, &d->outputEvents);

    const float sourceLeft = left;
    const float sourceRight = right;
    const float sourceDryLeft = alignedDryLeft;
    const float sourceDryRight = alignedDryRight;
    left = 0.0f;
    right = 0.0f;
    alignedDryLeft = 0.0f;
    alignedDryRight = 0.0f;
    if (d->outputReady)
    {
        left = d->descriptor.outputChannels == 1
            ? d->monoOutput[d->outputRead] : d->outputLeft[d->outputRead];
        right = d->descriptor.outputChannels == 1
            ? d->monoOutput[d->outputRead] : d->outputRight[d->outputRead];
        alignedDryLeft = d->dryLeft[d->outputRead];
        alignedDryRight = d->dryRight[d->outputRead];
        if (++d->outputRead >= clapBlockFrames)
        {
            d->outputReady = false;
            d->outputRead = 0;
        }
    }

    const uint32_t frame = d->bufferedFrames;
    d->inputLeft[frame] = d->kind == ClapPluginKind::AudioEffect ? sourceLeft : 0.0f;
    d->inputRight[frame] = d->kind == ClapPluginKind::AudioEffect ? sourceRight : 0.0f;
    d->monoInput[frame] = (d->inputLeft[frame] + d->inputRight[frame]) * 0.5f;
    d->dryLeft[frame] = d->kind == ClapPluginKind::AudioEffect ? sourceDryLeft : 0.0f;
    d->dryRight[frame] = d->kind == ClapPluginKind::AudioEffect ? sourceDryRight : 0.0f;
    ++d->bufferedFrames;

    if (d->bufferedFrames == clapBlockFrames)
    {
        d->outputLeft.fill(0.0f);
        d->outputRight.fill(0.0f);
        d->monoOutput.fill(0.0f);
        clap_audio_buffer_t inputBuffer{};
        inputBuffer.data32 = d->kind == ClapPluginKind::Instrument ? nullptr : d->inputChannels;
        inputBuffer.channel_count = d->descriptor.inputChannels;
        clap_audio_buffer_t outputBuffer{};
        outputBuffer.data32 = d->outputChannels;
        outputBuffer.channel_count = d->descriptor.outputChannels;

        clap_process_t process{};
        process.steady_time = d->steadyTime;
        d->steadyTime += clapBlockFrames;
        process.frames_count = clapBlockFrames;
        process.audio_inputs = d->kind == ClapPluginKind::Instrument ? nullptr : &inputBuffer;
        process.audio_outputs = &outputBuffer;
        process.audio_inputs_count = d->kind == ClapPluginKind::Instrument ? 0 : 1;
        process.audio_outputs_count = 1;
        process.in_events = &d->inputEvents;
        process.out_events = &d->outputEvents;
        const clap_process_status status = d->plugin->process(d->plugin, &process);
        d->pendingEvents.clear();
        d->pendingInstrumentEventCount = 0;
        if (status == CLAP_PROCESS_ERROR)
        {
            d->outputLeft.fill(0.0f);
            d->outputRight.fill(0.0f);
            d->monoOutput.fill(0.0f);
            for (size_t index = 0; index < d->sentValues.size(); ++index)
                if (!d->descriptor.parameters[static_cast<qsizetype>(index)].readOnly)
                    d->sentValues[index] = std::numeric_limits<float>::quiet_NaN();
        }
        else
        {
            for (float& sample : d->outputLeft)
                if (!std::isfinite(sample)) sample = 0.0f;
            for (float& sample : d->outputRight)
                if (!std::isfinite(sample)) sample = 0.0f;
            for (float& sample : d->monoOutput)
                if (!std::isfinite(sample)) sample = 0.0f;
        }
        d->bufferedFrames = 0;
        d->outputRead = 0;
        d->outputReady = true;
    }
    return true;
#else
    Q_UNUSED(left)
    Q_UNUSED(right)
    Q_UNUSED(alignedDryLeft)
    Q_UNUSED(alignedDryRight)
    return false;
#endif
}
