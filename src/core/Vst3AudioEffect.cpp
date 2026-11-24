#include "Vst3AudioEffect.h"

#include "Project.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QIODevice>

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/utility/memoryibstream.h>
#include <public.sdk/source/vst/utility/midiconvert.h>
#include <public.sdk/source/vst/utility/stringconvert.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <public.sdk/source/vst/utility/uid.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Vst3Module = VST3::Hosting::Module;
using Vst3ClassInfo = VST3::Hosting::ClassInfo;
using Vst3Factory = VST3::Hosting::PluginFactory;
using Vst3Component = Steinberg::Vst::IComponent;
using Vst3Controller = Steinberg::Vst::IEditController;
using Vst3Processor = Steinberg::Vst::IAudioProcessor;

class AwesomeHostApplication final : public Steinberg::Vst::HostApplication
{
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override
    {
        return Steinberg::Vst::StringConvert::convert("Awesome Studio", name)
            ? Steinberg::kResultTrue : Steinberg::kInternalError;
    }
};

class AudioComponentHandler final : public Steinberg::Vst::IComponentHandler
{
public:
    explicit AudioComponentHandler(std::atomic<Steinberg::int32>& pendingRestartFlags)
        : pendingRestartFlags(pendingRestartFlags)
    {
    }

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API performEdit(
        Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue) override
    {
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kNotImplemented;
    }

    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override
    {
        if ((flags & Steinberg::Vst::kLatencyChanged) == 0)
            return Steinberg::kNotImplemented;
        pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                     std::memory_order_release);
        return flags == Steinberg::Vst::kLatencyChanged
            ? Steinberg::kResultTrue : Steinberg::kNotImplemented;
    }

    Steinberg::uint32 PLUGIN_API addRef() override
    {
        return ++referenceCount;
    }

    Steinberg::uint32 PLUGIN_API release() override
    {
        const Steinberg::uint32 remaining = --referenceCount;
        if (remaining == 0)
            delete this;
        return remaining;
    }

private:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                  void** object) override
    {
        if (object == nullptr)
            return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (Steinberg::FUnknownPrivate::iidEqual(
                iid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid))
        {
            *object = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            addRef();
            return Steinberg::kResultTrue;
        }
        return Steinberg::kNoInterface;
    }

    std::atomic<Steinberg::uint32> referenceCount{1};
    std::atomic<Steinberg::int32>& pendingRestartFlags;
};

constexpr std::uint32_t maximumVst3Parameters = 256;
constexpr std::uint32_t vst3BlockFrames = 64;
constexpr int parameterCurveResolution = 256;
constexpr auto vst3StatePrefix = "VST3_STATE_V1:";

struct Vst3CatalogEntry
{
    Vst3PluginDescriptor descriptor;
    Vst3Module::Ptr module;
    Vst3ClassInfo classInfo;
};

Steinberg::FUnknown* hostContext()
{
    static auto* application = new AwesomeHostApplication();
    static const bool configured = []
    {
        Steinberg::Vst::PlugProvider::setErrorStream(nullptr);
        Steinberg::Vst::PluginContextFactory::instance().setPluginContext(application);
        return true;
    }();
    Q_UNUSED(configured)
    return application;
}

QString uidString(const Vst3ClassInfo& classInfo)
{
    return QString::fromStdString(classInfo.ID().toString(false));
}

float plainValueForNormalized(const Vst3PluginParameter& parameter, double normalized)
{
    if (parameter.plainValuesByNormalized.isEmpty())
        return parameter.defaultValue;
    const double position = std::clamp(normalized, 0.0, 1.0) * parameterCurveResolution;
    const int lowerIndex = std::clamp(static_cast<int>(std::floor(position)),
                                      0, parameterCurveResolution);
    const int upperIndex = std::min(lowerIndex + 1, parameterCurveResolution);
    const float amount = static_cast<float>(position - lowerIndex);
    const float lower = parameter.plainValuesByNormalized[lowerIndex];
    const float upper = parameter.plainValuesByNormalized[upperIndex];
    return lower + (upper - lower) * amount;
}

double normalizedValueForPlain(const Vst3PluginParameter& parameter, float plain)
{
    const QVector<float>& values = parameter.plainValuesByNormalized;
    if (values.size() < 2)
        return 0.0;
    for (int index = 1; index < values.size(); ++index)
    {
        const float previous = values[index - 1];
        const float current = values[index];
        const float lower = std::min(previous, current);
        const float upper = std::max(previous, current);
        if (plain < lower || plain > upper)
            continue;
        const float span = current - previous;
        const double fraction = std::abs(span) < 1.0e-20f
            ? 0.0 : static_cast<double>((plain - previous) / span);
        return std::clamp((index - 1 + fraction) / parameterCurveResolution, 0.0, 1.0);
    }
    return plain <= parameter.minimum ? 0.0 : 1.0;
}

bool inspectPlugin(const Vst3Module::Ptr& module, const Vst3ClassInfo& classInfo,
                   Vst3PluginDescriptor& descriptor, bool instrument)
{
    if (!module || classInfo.category() != kVstAudioEffectClass)
        return false;
    const auto& categories = classInfo.subCategories();
    const bool instrumentClass = std::any_of(categories.cbegin(), categories.cend(),
        [](const std::string& category)
        {
            return category == "Instrument" || category.rfind("Instrument|", 0) == 0 ||
                   category == "Fx|Instrument" || category.rfind("Fx|Instrument|", 0) == 0;
        });
    if (instrument != instrumentClass)
        return false;

    const Vst3Factory factory = module->getFactory();
    factory.setHostContext(hostContext());
    auto provider = Steinberg::owned(new Steinberg::Vst::PlugProvider(factory, classInfo, true));
    if (!provider->initialize())
        return false;

    Steinberg::IPtr<Vst3Component> component = provider->getComponentPtr();
    Steinberg::IPtr<Vst3Controller> controller = provider->getControllerPtr();
    Steinberg::FUnknownPtr<Vst3Processor> processor(component);
    if (!component || !processor ||
        component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) !=
            (instrument ? 0 : 1) ||
        component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) != 1)
        return false;

    Steinberg::Vst::BusInfo inputInfo{};
    Steinberg::Vst::BusInfo outputInfo{};
    if ((!instrument && component->getBusInfo(
             Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, inputInfo) !=
                 Steinberg::kResultTrue) ||
        component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, outputInfo) !=
            Steinberg::kResultTrue ||
        (!instrument && inputInfo.busType != Steinberg::Vst::kMain) ||
        outputInfo.busType != Steinberg::Vst::kMain ||
        (!instrument && (inputInfo.channelCount < 1 || inputInfo.channelCount > 2)) ||
        outputInfo.channelCount < 1 || outputInfo.channelCount > 2)
        return false;

    if (instrument)
    {
        if (component->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput) != 1)
            return false;
        Steinberg::Vst::BusInfo eventInfo{};
        if (component->getBusInfo(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, eventInfo) !=
                Steinberg::kResultTrue || eventInfo.busType != Steinberg::Vst::kMain)
            return false;
    }

    descriptor.id = uidString(classInfo);
    descriptor.name = QString::fromStdString(classInfo.name());
    descriptor.vendor = QString::fromStdString(classInfo.vendor());
    descriptor.path = QString::fromUtf8(module->getPath().data(),
                                        static_cast<qsizetype>(module->getPath().size()));
    descriptor.instrument = instrument;
    descriptor.inputChannels = instrument ? 0 : static_cast<std::uint32_t>(inputInfo.channelCount);
    descriptor.outputChannels = static_cast<std::uint32_t>(outputInfo.channelCount);
    descriptor.parameters.clear();

    if (!controller)
        return true;
    const Steinberg::int32 count = std::clamp<Steinberg::int32>(
        controller->getParameterCount(), 0, static_cast<Steinberg::int32>(maximumVst3Parameters));
    descriptor.parameters.reserve(count);
    for (Steinberg::int32 index = 0; index < count; ++index)
    {
        Steinberg::Vst::ParameterInfo info{};
        if (controller->getParameterInfo(index, info) != Steinberg::kResultTrue ||
            info.id == Steinberg::Vst::kNoParamId)
            continue;

        Vst3PluginParameter parameter;
        parameter.id = info.id;
        parameter.name = QString::fromStdString(
            Steinberg::Vst::StringConvert::convert(info.title, 128));
        if (parameter.name.isEmpty())
            parameter.name = QStringLiteral("Parameter %1").arg(index + 1);
        parameter.integer = info.stepCount > 0;
        parameter.toggled = info.stepCount == 1;
        parameter.hidden = (info.flags & Steinberg::Vst::ParameterInfo::kIsHidden) != 0;
        parameter.readOnly = (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0;
        parameter.plainValuesByNormalized.reserve(parameterCurveResolution + 1);
        bool validCurve = true;
        for (int sample = 0; sample <= parameterCurveResolution; ++sample)
        {
            const double normalized = static_cast<double>(sample) / parameterCurveResolution;
            const double plain = controller->normalizedParamToPlain(info.id, normalized);
            if (!std::isfinite(plain) || std::abs(plain) > 1.0e9)
            {
                validCurve = false;
                break;
            }
            parameter.plainValuesByNormalized.push_back(static_cast<float>(plain));
        }
        if (!validCurve || parameter.plainValuesByNormalized.size() != parameterCurveResolution + 1)
            continue;
        const auto range = std::minmax_element(parameter.plainValuesByNormalized.cbegin(),
                                               parameter.plainValuesByNormalized.cend());
        parameter.minimum = *range.first;
        parameter.maximum = *range.second;
        if (parameter.maximum <= parameter.minimum)
            continue;
        parameter.defaultValue = plainValueForNormalized(parameter, info.defaultNormalizedValue);
        descriptor.parameters.push_back(std::move(parameter));
    }
    return true;
}

const std::vector<Vst3CatalogEntry>& catalog()
{
    static std::once_flag once;
    static std::vector<Vst3CatalogEntry> entries;
    std::call_once(once, []
    {
        for (const std::string& path : Vst3Module::getModulePaths())
        {
            std::string error;
            const auto module = Vst3Module::create(path, error);
            if (!module)
                continue;
            const Vst3Factory factory = module->getFactory();
            factory.setHostContext(hostContext());
            for (const Vst3ClassInfo& classInfo : factory.classInfos())
            {
                for (bool instrument : {false, true})
                {
                    Vst3PluginDescriptor descriptor;
                    if (!inspectPlugin(module, classInfo, descriptor, instrument))
                        continue;
                    const bool duplicate = std::any_of(entries.cbegin(), entries.cend(),
                        [&descriptor](const Vst3CatalogEntry& item)
                        {
                            return item.descriptor.id == descriptor.id &&
                                   item.descriptor.instrument == descriptor.instrument;
                        });
                    if (!duplicate)
                        entries.push_back({std::move(descriptor), module, classInfo});
                }
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
        {
            if (left.descriptor.name != right.descriptor.name)
                return left.descriptor.name < right.descriptor.name;
            return left.descriptor.vendor < right.descriptor.vendor;
        });
    });
    return entries;
}

const Vst3CatalogEntry* catalogEntry(const QString& id, bool instrument = false)
{
    const auto& entries = catalog();
    const auto entry = std::find_if(entries.cbegin(), entries.cend(), [&id, instrument](const auto& value)
    {
        return value.descriptor.instrument == instrument &&
               value.descriptor.id.compare(id, Qt::CaseInsensitive) == 0;
    });
    return entry == entries.cend() ? nullptr : &*entry;
}

bool loadStream(const QByteArray& bytes, Steinberg::ResizableMemoryIBStream& stream)
{
    if (bytes.isEmpty())
        return true;
    if (stream.write(const_cast<char*>(bytes.constData()), static_cast<Steinberg::int32>(bytes.size()),
                     nullptr) != Steinberg::kResultTrue)
        return false;
    stream.rewind();
    return true;
}

void updateProcessContext(const Vst3TransportContext& transport,
                          Steinberg::Vst::ProcessContext& context)
{
    context = {};
    context.sampleRate = transport.sampleRate;
    context.projectTimeSamples = transport.projectTimeSamples;
    context.projectTimeMusic = transport.projectTimeMusic;
    context.tempo = transport.tempo;
    context.timeSigNumerator = transport.timeSigNumerator;
    context.timeSigDenominator = transport.timeSigDenominator;
    const double quarterNotesPerBar = transport.timeSigNumerator * 4.0 /
        std::max(1, transport.timeSigDenominator);
    context.barPositionMusic = std::floor(transport.projectTimeMusic / quarterNotesPerBar) *
        quarterNotesPerBar;
    context.state = Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
        Steinberg::Vst::ProcessContext::kBarPositionValid |
        Steinberg::Vst::ProcessContext::kTempoValid |
        Steinberg::Vst::ProcessContext::kTimeSigValid;
    if (transport.playing)
        context.state |= Steinberg::Vst::ProcessContext::kPlaying;
    if (transport.cycleActive)
    {
        context.state |= Steinberg::Vst::ProcessContext::kCycleActive |
            Steinberg::Vst::ProcessContext::kCycleValid;
        context.cycleStartMusic = transport.cycleStartMusic;
        context.cycleEndMusic = transport.cycleEndMusic;
    }
}

QByteArray streamBytes(const Steinberg::ResizableMemoryIBStream& stream)
{
    return QByteArray(static_cast<const char*>(stream.getData()),
                      static_cast<qsizetype>(stream.getCursor()));
}
}

struct Vst3AudioEffectInstance::Private
{
    Vst3Module::Ptr module;
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider;
    Steinberg::IPtr<Vst3Component> component;
    Steinberg::IPtr<Vst3Controller> controller;
    Steinberg::IPtr<Vst3Processor> processor;
    std::atomic<Steinberg::int32> pendingRestartFlags{0};
    Steinberg::IPtr<AudioComponentHandler> componentHandler;
    Vst3PluginDescriptor descriptor;
    Steinberg::Vst::ParameterChanges parameterChanges;
    std::vector<float> controlValues;
    std::vector<double> normalizedValues;
    std::vector<double> lastSentValues;
    Steinberg::Vst::ProcessContext processContext{};
    std::array<float, vst3BlockFrames> inputLeft{};
    std::array<float, vst3BlockFrames> inputRight{};
    std::array<float, vst3BlockFrames> monoInput{};
    std::array<float, vst3BlockFrames> outputLeft{};
    std::array<float, vst3BlockFrames> outputRight{};
    std::array<float, vst3BlockFrames> monoOutput{};
    std::array<float, vst3BlockFrames> dryLeft{};
    std::array<float, vst3BlockFrames> dryRight{};
    std::uint32_t inputChannels = 0;
    std::uint32_t outputChannels = 0;
    std::uint32_t bufferedFrames = 0;
    std::uint32_t outputRead = 0;
    std::uint32_t reportedLatencyFrames = 0;
    bool outputReady = false;
    bool active = false;
    bool processing = false;

    ~Private()
    {
        if (controller)
            controller->setComponentHandler(nullptr);
        if (processing && processor)
            processor->setProcessing(false);
        if (active && component)
            component->setActive(false);
    }

    bool serviceLatencyRestart()
    {
        const Steinberg::int32 flags = pendingRestartFlags.exchange(
            0, std::memory_order_acq_rel);
        if ((flags & Steinberg::Vst::kLatencyChanged) == 0 || !component || !processor)
            return false;

        const qint64 previousLatency = active
            ? static_cast<qint64>(vst3BlockFrames) + reportedLatencyFrames : 0;
        if (processing)
        {
            if (processor->setProcessing(false) != Steinberg::kResultTrue)
            {
                pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                             std::memory_order_release);
                return false;
            }
            processing = false;
        }
        if (active && component->setActive(false) != Steinberg::kResultTrue)
        {
            if (processor->setProcessing(true) == Steinberg::kResultTrue)
                processing = true;
            pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                         std::memory_order_release);
            return false;
        }
        active = false;
        if (component->setActive(true) != Steinberg::kResultTrue)
        {
            pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                         std::memory_order_release);
            return previousLatency != 0;
        }
        active = true;
        reportedLatencyFrames = processor->getLatencySamples();
        bufferedFrames = 0;
        outputRead = 0;
        outputReady = false;
        inputLeft.fill(0.0f);
        inputRight.fill(0.0f);
        monoInput.fill(0.0f);
        outputLeft.fill(0.0f);
        outputRight.fill(0.0f);
        monoOutput.fill(0.0f);
        dryLeft.fill(0.0f);
        dryRight.fill(0.0f);
        std::fill(lastSentValues.begin(), lastSentValues.end(),
                  std::numeric_limits<double>::quiet_NaN());
        return true;
    }

    void restoreState(const QString& state)
    {
        if (state.isEmpty() || !state.startsWith(QLatin1String(vst3StatePrefix)))
            return;
        const QByteArray payload = QByteArray::fromBase64(
            state.mid(static_cast<qsizetype>(std::char_traits<char>::length(vst3StatePrefix)))
                .toLatin1());
        QDataStream stream(payload);
        stream.setByteOrder(QDataStream::LittleEndian);
        quint32 componentSize = 0;
        quint32 controllerSize = 0;
        stream >> componentSize;
        if (stream.status() != QDataStream::Ok || componentSize > maximumPluginStateBytes)
            return;
        QByteArray componentBytes(static_cast<qsizetype>(componentSize), Qt::Uninitialized);
        if (componentSize > 0 && stream.readRawData(componentBytes.data(),
                static_cast<int>(componentSize)) != static_cast<int>(componentSize))
            return;
        stream >> controllerSize;
        if (stream.status() != QDataStream::Ok || controllerSize > maximumPluginStateBytes ||
            static_cast<quint64>(componentSize) + controllerSize > maximumPluginStateBytes)
            return;
        QByteArray controllerBytes(static_cast<qsizetype>(controllerSize), Qt::Uninitialized);
        if (controllerSize > 0 && stream.readRawData(controllerBytes.data(),
                static_cast<int>(controllerSize)) != static_cast<int>(controllerSize))
            return;

        Steinberg::ResizableMemoryIBStream componentStream(componentBytes.size());
        Steinberg::ResizableMemoryIBStream controllerStream(controllerBytes.size());
        if (!loadStream(componentBytes, componentStream) ||
            !loadStream(controllerBytes, controllerStream))
            return;
        if (!componentBytes.isEmpty())
        {
            component->setState(&componentStream);
            componentStream.rewind();
            if (controller)
                controller->setComponentState(&componentStream);
        }
        if (controller && !controllerBytes.isEmpty())
            controller->setState(&controllerStream);
    }

    void prepareParameterQueues()
    {
        parameterChanges.setMaxParameters(static_cast<Steinberg::int32>(descriptor.parameters.size()));
        for (const Vst3PluginParameter& parameter : descriptor.parameters)
        {
            Steinberg::int32 queueIndex = 0;
            auto* queue = parameterChanges.addParameterData(parameter.id, queueIndex);
            if (queue == nullptr)
                continue;
            for (Steinberg::int32 frame = 0; frame < static_cast<Steinberg::int32>(vst3BlockFrames); ++frame)
            {
                Steinberg::int32 pointIndex = 0;
                queue->addPoint(frame, 0.0, pointIndex);
            }
            static_cast<Steinberg::Vst::ParameterValueQueue*>(queue)->clear();
        }
        parameterChanges.clearQueue();
    }

    void processBlock()
    {
        outputLeft.fill(0.0f);
        outputRight.fill(0.0f);
        monoOutput.fill(0.0f);

        Steinberg::Vst::Sample32* inputPointers[2] = {
            inputChannels == 1 ? monoInput.data() : inputLeft.data(), inputRight.data()
        };
        Steinberg::Vst::Sample32* outputPointers[2] = {
            outputChannels == 1 ? monoOutput.data() : outputLeft.data(), outputRight.data()
        };
        Steinberg::Vst::AudioBusBuffers inputBus{};
        inputBus.numChannels = static_cast<Steinberg::int32>(inputChannels);
        inputBus.channelBuffers32 = inputPointers;
        Steinberg::Vst::AudioBusBuffers outputBus{};
        outputBus.numChannels = static_cast<Steinberg::int32>(outputChannels);
        outputBus.channelBuffers32 = outputPointers;

        Steinberg::Vst::ProcessData data{};
        data.processMode = Steinberg::Vst::kRealtime;
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = static_cast<Steinberg::int32>(vst3BlockFrames);
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inputBus;
        data.outputs = &outputBus;
        data.inputParameterChanges = &parameterChanges;
        data.processContext = &processContext;
        const bool processed = processor && processor->process(data) == Steinberg::kResultTrue;
        parameterChanges.clearQueue();
        if (!processed)
        {
            outputLeft.fill(0.0f);
            outputRight.fill(0.0f);
            monoOutput.fill(0.0f);
            std::fill(lastSentValues.begin(), lastSentValues.end(),
                      std::numeric_limits<double>::quiet_NaN());
        }
        else
        {
            for (float& sample : outputLeft)
                if (!std::isfinite(sample)) sample = 0.0f;
            for (float& sample : outputRight)
                if (!std::isfinite(sample)) sample = 0.0f;
            for (float& sample : monoOutput)
                if (!std::isfinite(sample)) sample = 0.0f;
        }
        bufferedFrames = 0;
        outputRead = 0;
        outputReady = true;
    }
};

QVector<Vst3PluginDescriptor> Vst3AudioEffectInstance::availableEffects()
{
    QVector<Vst3PluginDescriptor> result;
    const auto& entries = catalog();
    result.reserve(static_cast<qsizetype>(entries.size()));
    for (const Vst3CatalogEntry& entry : entries)
        if (!entry.descriptor.instrument)
            result.push_back(entry.descriptor);
    return result;
}

bool Vst3AudioEffectInstance::describe(const QString& id, Vst3PluginDescriptor& descriptor)
{
    const Vst3CatalogEntry* entry = catalogEntry(id, false);
    if (entry == nullptr)
        return false;
    descriptor = entry->descriptor;
    return true;
}

QVector<Vst3PluginDescriptor> Vst3AudioInstrumentInstance::availableInstruments()
{
    QVector<Vst3PluginDescriptor> result;
    const auto& entries = catalog();
    result.reserve(static_cast<qsizetype>(entries.size()));
    for (const Vst3CatalogEntry& entry : entries)
        if (entry.descriptor.instrument)
            result.push_back(entry.descriptor);
    return result;
}

bool Vst3AudioInstrumentInstance::describe(const QString& id,
                                           Vst3PluginDescriptor& descriptor)
{
    const Vst3CatalogEntry* entry = catalogEntry(id, true);
    if (entry == nullptr)
        return false;
    descriptor = entry->descriptor;
    return true;
}

Vst3AudioEffectInstance::Vst3AudioEffectInstance(const QString& id, double sampleRate,
                                                 const QVector<float>& parameterValues,
                                                 const QString& pluginState)
    : d(std::make_unique<Private>())
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return;
    const Vst3CatalogEntry* entry = catalogEntry(id, false);
    if (entry == nullptr)
        return;

    d->module = entry->module;
    d->descriptor = entry->descriptor;
    d->inputChannels = d->descriptor.inputChannels;
    d->outputChannels = d->descriptor.outputChannels;
    const Vst3Factory factory = d->module->getFactory();
    factory.setHostContext(hostContext());
    d->provider = Steinberg::owned(new Steinberg::Vst::PlugProvider(factory, entry->classInfo, true));
    if (!d->provider->initialize())
        return;
    d->component = d->provider->getComponentPtr();
    d->controller = d->provider->getControllerPtr();
    if (d->controller)
    {
        d->componentHandler = Steinberg::owned(
            new AudioComponentHandler(d->pendingRestartFlags));
        d->controller->setComponentHandler(d->componentHandler);
    }
    d->processor = Steinberg::FUnknownPtr<Vst3Processor>(d->component);
    if (!d->component || !d->processor ||
        d->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true) !=
            Steinberg::kResultTrue ||
        d->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true) !=
            Steinberg::kResultTrue ||
        d->processor->canProcessSampleSize(Steinberg::Vst::kSample32) != Steinberg::kResultTrue)
        return;

    d->restoreState(pluginState);
    d->controlValues.resize(static_cast<size_t>(d->descriptor.parameters.size()));
    d->normalizedValues.resize(d->controlValues.size(), 0.0);
    d->lastSentValues.resize(d->controlValues.size(), std::numeric_limits<double>::quiet_NaN());
    for (qsizetype index = 0; index < d->descriptor.parameters.size(); ++index)
    {
        const Vst3PluginParameter& parameter = d->descriptor.parameters[index];
        double normalized = d->controller
            ? d->controller->getParamNormalized(parameter.id) : 0.0;
        if (!std::isfinite(normalized))
            normalized = normalizedValueForPlain(parameter, parameter.defaultValue);
        float value = index < parameterValues.size()
            ? parameterValues[index] : plainValueForNormalized(parameter, normalized);
        if (!std::isfinite(value))
            value = parameter.defaultValue;
        value = std::clamp(value, parameter.minimum, parameter.maximum);
        if (parameter.toggled)
            value = value >= 0.5f ? 1.0f : 0.0f;
        else if (parameter.integer)
            value = std::round(value);
        d->controlValues[static_cast<size_t>(index)] = value;
        d->normalizedValues[static_cast<size_t>(index)] = normalizedValueForPlain(parameter, value);
    }
    d->prepareParameterQueues();

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(vst3BlockFrames);
    setup.sampleRate = sampleRate;
    if (d->processor->setupProcessing(setup) != Steinberg::kResultTrue ||
        d->component->setActive(true) != Steinberg::kResultTrue)
        return;
    d->active = true;
    d->reportedLatencyFrames = d->processor->getLatencySamples();
    if (d->processor->setProcessing(true) != Steinberg::kResultTrue)
    {
        d->component->setActive(false);
        d->active = false;
        return;
    }
    d->processing = true;
}

Vst3AudioEffectInstance::~Vst3AudioEffectInstance() = default;

bool Vst3AudioEffectInstance::isValid() const
{
    return d != nullptr && d->active && d->processor;
}

bool Vst3AudioEffectInstance::hasPendingLatencyRestart() const
{
    return d != nullptr &&
        (d->pendingRestartFlags.load(std::memory_order_acquire) &
         Steinberg::Vst::kLatencyChanged) != 0;
}

bool Vst3AudioEffectInstance::serviceLatencyRestart()
{
    return d != nullptr && d->serviceLatencyRestart();
}

qint64 Vst3AudioEffectInstance::processingLatencyFrames() const
{
    return isValid() ? static_cast<qint64>(vst3BlockFrames) + d->reportedLatencyFrames : 0;
}

qsizetype Vst3AudioEffectInstance::parameterCount() const
{
    return d == nullptr ? 0 : static_cast<qsizetype>(d->controlValues.size());
}

float Vst3AudioEffectInstance::parameterValue(qsizetype index) const
{
    if (d == nullptr || index < 0 || index >= static_cast<qsizetype>(d->controlValues.size()))
        return 0.0f;
    return d->controlValues[static_cast<size_t>(index)];
}

void Vst3AudioEffectInstance::setParameter(qsizetype index, float value)
{
    if (d == nullptr || !d->controller || index < 0 ||
        index >= static_cast<qsizetype>(d->controlValues.size()))
        return;
    const Vst3PluginParameter& parameter = d->descriptor.parameters[index];
    if (parameter.readOnly)
        return;
    if (!std::isfinite(value))
        value = parameter.defaultValue;
    value = std::clamp(value, parameter.minimum, parameter.maximum);
    if (parameter.toggled)
        value = value >= 0.5f ? 1.0f : 0.0f;
    else if (parameter.integer)
        value = std::round(value);
    d->controlValues[static_cast<size_t>(index)] = value;
    d->normalizedValues[static_cast<size_t>(index)] = normalizedValueForPlain(parameter, value);
}

bool Vst3AudioEffectInstance::saveState(QString& pluginState) const
{
    if (!isValid())
        return false;
    Steinberg::ResizableMemoryIBStream componentStream;
    Steinberg::ResizableMemoryIBStream controllerStream;
    const bool componentSaved = d->component->getState(&componentStream) == Steinberg::kResultTrue;
    const bool controllerSaved = d->controller &&
        d->controller->getState(&controllerStream) == Steinberg::kResultTrue;
    if (!componentSaved && !controllerSaved)
        return false;

    const QByteArray componentBytes = streamBytes(componentStream);
    const QByteArray controllerBytes = streamBytes(controllerStream);
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << static_cast<quint32>(componentBytes.size());
    if (!componentBytes.isEmpty())
        stream.writeRawData(componentBytes.constData(), componentBytes.size());
    stream << static_cast<quint32>(controllerBytes.size());
    if (!controllerBytes.isEmpty())
        stream.writeRawData(controllerBytes.constData(), controllerBytes.size());
    const QByteArray encoded = payload.toBase64();
    const QByteArray prefix(vst3StatePrefix);
    if (stream.status() != QDataStream::Ok || encoded.size() + prefix.size() > maximumPluginStateBytes)
        return false;
    pluginState = QString::fromLatin1(prefix + encoded);
    return true;
}

void Vst3AudioEffectInstance::stopProcessing()
{
    if (d != nullptr && d->processing && d->processor)
    {
        d->processor->setProcessing(false);
        d->processing = false;
    }
}

void Vst3AudioEffectInstance::resetProcessingState()
{
    if (d == nullptr)
        return;
    d->bufferedFrames = 0;
    d->outputRead = 0;
    d->outputReady = false;
    d->inputLeft.fill(0.0f);
    d->inputRight.fill(0.0f);
    d->monoInput.fill(0.0f);
    d->outputLeft.fill(0.0f);
    d->outputRight.fill(0.0f);
    d->monoOutput.fill(0.0f);
    d->dryLeft.fill(0.0f);
    d->dryRight.fill(0.0f);
    std::fill(d->lastSentValues.begin(), d->lastSentValues.end(),
              std::numeric_limits<double>::quiet_NaN());
    if (d->processing && d->processor)
    {
        d->processor->setProcessing(false);
        d->processing = false;
    }
}

bool Vst3AudioEffectInstance::process(float& left, float& right,
                                      float& alignedDryLeft, float& alignedDryRight,
                                      const Vst3TransportContext& transportContext)
{
    if (!isValid())
        return false;

    if (d->bufferedFrames == 0)
        updateProcessContext(transportContext, d->processContext);

    if (!d->processing)
    {
        if (d->processor->setProcessing(true) != Steinberg::kResultTrue)
            return false;
        d->processing = true;
    }

    for (size_t index = 0; index < d->normalizedValues.size(); ++index)
    {
        const double value = d->normalizedValues[index];
        if (std::isfinite(d->lastSentValues[index]) &&
            std::abs(d->lastSentValues[index] - value) <= 1.0e-10)
            continue;
        Steinberg::int32 queueIndex = 0;
        auto* queue = d->parameterChanges.addParameterData(
            d->descriptor.parameters[static_cast<qsizetype>(index)].id, queueIndex);
        if (queue == nullptr)
            continue;
        Steinberg::int32 pointIndex = 0;
        queue->addPoint(static_cast<Steinberg::int32>(d->bufferedFrames), value, pointIndex);
        d->lastSentValues[index] = value;
    }

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
        left = d->outputChannels == 1 ? d->monoOutput[d->outputRead]
                                      : d->outputLeft[d->outputRead];
        right = d->outputChannels == 1 ? d->monoOutput[d->outputRead]
                                       : d->outputRight[d->outputRead];
        alignedDryLeft = d->dryLeft[d->outputRead];
        alignedDryRight = d->dryRight[d->outputRead];
        if (++d->outputRead >= vst3BlockFrames)
        {
            d->outputReady = false;
            d->outputRead = 0;
        }
    }

    const std::uint32_t frame = d->bufferedFrames;
    d->inputLeft[frame] = sourceLeft;
    d->inputRight[frame] = sourceRight;
    d->monoInput[frame] = (sourceLeft + sourceRight) * 0.5f;
    d->dryLeft[frame] = sourceDryLeft;
    d->dryRight[frame] = sourceDryRight;
    if (++d->bufferedFrames == vst3BlockFrames)
        d->processBlock();
    return true;
}

struct Vst3AudioInstrumentInstance::Private
{
    struct ActiveNote
    {
        std::int32_t id = 0;
        int channel = 0;
        int key = 0;
        bool active = false;
    };

    Vst3Module::Ptr module;
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider;
    Steinberg::IPtr<Vst3Component> component;
    Steinberg::IPtr<Vst3Controller> controller;
    Steinberg::IPtr<Vst3Processor> processor;
    std::atomic<Steinberg::int32> pendingRestartFlags{0};
    Steinberg::IPtr<AudioComponentHandler> componentHandler;
    Vst3PluginDescriptor descriptor;
    Steinberg::Vst::ParameterChanges parameterChanges;
    Steinberg::Vst::EventList inputEvents{1024};
    std::vector<float> controlValues;
    std::vector<double> normalizedValues;
    std::vector<double> lastSentValues;
    Steinberg::Vst::ProcessContext processContext{};
    std::array<Steinberg::Vst::ParamID, 16> pitchBendParameterIds{};
    std::array<double, 16> pitchBendNormalizedValues{};
    std::array<std::uint64_t, 16> pitchBendChangeOrder{};
    std::uint64_t nextPitchBendChangeOrder = 0;
    std::array<float, vst3BlockFrames> outputLeft{};
    std::array<float, vst3BlockFrames> outputRight{};
    std::array<float, vst3BlockFrames> monoOutput{};
    std::array<ActiveNote, 512> activeNotes{};
    std::uint32_t outputChannels = 0;
    std::uint32_t bufferedFrames = 0;
    std::uint32_t outputRead = 0;
    std::uint32_t reportedLatencyFrames = 0;
    bool outputReady = false;
    bool active = false;
    bool processing = false;

    ~Private()
    {
        if (controller)
            controller->setComponentHandler(nullptr);
        if (processing && processor)
            processor->setProcessing(false);
        if (active && component)
            component->setActive(false);
    }

    bool serviceLatencyRestart()
    {
        const Steinberg::int32 flags = pendingRestartFlags.exchange(
            0, std::memory_order_acq_rel);
        if ((flags & Steinberg::Vst::kLatencyChanged) == 0 || !component || !processor)
            return false;

        const qint64 previousLatency = active
            ? static_cast<qint64>(vst3BlockFrames) + reportedLatencyFrames : 0;
        if (processing)
        {
            if (processor->setProcessing(false) != Steinberg::kResultTrue)
            {
                pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                             std::memory_order_release);
                return false;
            }
            processing = false;
        }
        if (active && component->setActive(false) != Steinberg::kResultTrue)
        {
            if (processor->setProcessing(true) == Steinberg::kResultTrue)
                processing = true;
            pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                         std::memory_order_release);
            return false;
        }
        active = false;
        if (component->setActive(true) != Steinberg::kResultTrue)
        {
            pendingRestartFlags.fetch_or(Steinberg::Vst::kLatencyChanged,
                                         std::memory_order_release);
            return previousLatency != 0;
        }
        active = true;
        reportedLatencyFrames = processor->getLatencySamples();
        bufferedFrames = 0;
        outputRead = 0;
        outputReady = false;
        outputLeft.fill(0.0f);
        outputRight.fill(0.0f);
        monoOutput.fill(0.0f);
        inputEvents.clear();
        activeNotes.fill(ActiveNote{});
        std::fill(lastSentValues.begin(), lastSentValues.end(),
                  std::numeric_limits<double>::quiet_NaN());
        return true;
    }

    void restoreState(const QString& state)
    {
        if (state.isEmpty() || !state.startsWith(QLatin1String(vst3StatePrefix)))
            return;
        const QByteArray payload = QByteArray::fromBase64(
            state.mid(static_cast<qsizetype>(std::char_traits<char>::length(vst3StatePrefix)))
                .toLatin1());
        QDataStream stream(payload);
        stream.setByteOrder(QDataStream::LittleEndian);
        quint32 componentSize = 0;
        quint32 controllerSize = 0;
        stream >> componentSize;
        if (stream.status() != QDataStream::Ok || componentSize > maximumPluginStateBytes)
            return;
        QByteArray componentBytes(static_cast<qsizetype>(componentSize), Qt::Uninitialized);
        if (componentSize > 0 && stream.readRawData(componentBytes.data(),
                static_cast<int>(componentSize)) != static_cast<int>(componentSize))
            return;
        stream >> controllerSize;
        if (stream.status() != QDataStream::Ok || controllerSize > maximumPluginStateBytes ||
            static_cast<quint64>(componentSize) + controllerSize > maximumPluginStateBytes)
            return;
        QByteArray controllerBytes(static_cast<qsizetype>(controllerSize), Qt::Uninitialized);
        if (controllerSize > 0 && stream.readRawData(controllerBytes.data(),
                static_cast<int>(controllerSize)) != static_cast<int>(controllerSize))
            return;

        Steinberg::ResizableMemoryIBStream componentStream(componentBytes.size());
        Steinberg::ResizableMemoryIBStream controllerStream(controllerBytes.size());
        if (!loadStream(componentBytes, componentStream) ||
            !loadStream(controllerBytes, controllerStream))
            return;
        if (!componentBytes.isEmpty())
        {
            component->setState(&componentStream);
            componentStream.rewind();
            if (controller)
                controller->setComponentState(&componentStream);
        }
        if (controller && !controllerBytes.isEmpty())
            controller->setState(&controllerStream);
    }

    void prepareParameterQueues()
    {
        parameterChanges.setMaxParameters(static_cast<Steinberg::int32>(descriptor.parameters.size()));
        for (const Vst3PluginParameter& parameter : descriptor.parameters)
        {
            Steinberg::int32 queueIndex = 0;
            auto* queue = parameterChanges.addParameterData(parameter.id, queueIndex);
            if (queue == nullptr)
                continue;
            for (Steinberg::int32 frame = 0; frame < static_cast<Steinberg::int32>(vst3BlockFrames); ++frame)
            {
                Steinberg::int32 pointIndex = 0;
                queue->addPoint(frame, 0.0, pointIndex);
            }
            static_cast<Steinberg::Vst::ParameterValueQueue*>(queue)->clear();
        }
        parameterChanges.clearQueue();
    }

    bool pushEvent(Steinberg::Vst::Event event)
    {
        event.busIndex = 0;
        event.sampleOffset = static_cast<Steinberg::int32>(
            std::min<std::uint32_t>(bufferedFrames, vst3BlockFrames - 1));
        event.ppqPosition = processContext.projectTimeMusic +
            static_cast<double>(event.sampleOffset) * processContext.tempo /
                (60.0 * std::max(1.0, processContext.sampleRate));
        return inputEvents.addEvent(event) == Steinberg::kResultTrue;
    }

    void processBlock()
    {
        outputLeft.fill(0.0f);
        outputRight.fill(0.0f);
        monoOutput.fill(0.0f);

        Steinberg::Vst::Sample32* outputPointers[2] = {
            outputChannels == 1 ? monoOutput.data() : outputLeft.data(), outputRight.data()
        };
        Steinberg::Vst::AudioBusBuffers outputBus{};
        outputBus.numChannels = static_cast<Steinberg::int32>(outputChannels);
        outputBus.channelBuffers32 = outputPointers;

        Steinberg::Vst::ProcessData data{};
        data.processMode = Steinberg::Vst::kRealtime;
        data.symbolicSampleSize = Steinberg::Vst::kSample32;
        data.numSamples = static_cast<Steinberg::int32>(vst3BlockFrames);
        data.numInputs = 0;
        data.numOutputs = 1;
        data.outputs = &outputBus;
        data.inputParameterChanges = &parameterChanges;
        data.inputEvents = &inputEvents;
        data.processContext = &processContext;
        const bool processed = processor && processor->process(data) == Steinberg::kResultTrue;
        parameterChanges.clearQueue();
        inputEvents.clear();
        if (!processed)
        {
            outputLeft.fill(0.0f);
            outputRight.fill(0.0f);
            monoOutput.fill(0.0f);
            std::fill(lastSentValues.begin(), lastSentValues.end(),
                      std::numeric_limits<double>::quiet_NaN());
        }
        else
        {
            for (float& sample : outputLeft)
                if (!std::isfinite(sample)) sample = 0.0f;
            for (float& sample : outputRight)
                if (!std::isfinite(sample)) sample = 0.0f;
            for (float& sample : monoOutput)
                if (!std::isfinite(sample)) sample = 0.0f;
        }
        bufferedFrames = 0;
        outputRead = 0;
        outputReady = true;
    }
};

Vst3AudioInstrumentInstance::Vst3AudioInstrumentInstance(
    const QString& id, double sampleRate, const QVector<float>& parameterValues,
    const QString& pluginState)
    : d(std::make_unique<Private>())
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return;
    const Vst3CatalogEntry* entry = catalogEntry(id, true);
    if (entry == nullptr)
        return;

    d->module = entry->module;
    d->descriptor = entry->descriptor;
    d->outputChannels = d->descriptor.outputChannels;
    d->pitchBendParameterIds.fill(Steinberg::Vst::kNoParamId);
    d->pitchBendNormalizedValues.fill(
        Steinberg::Vst::midi14BitToNormalized<double>(8192));
    const Vst3Factory factory = d->module->getFactory();
    factory.setHostContext(hostContext());
    d->provider = Steinberg::owned(new Steinberg::Vst::PlugProvider(factory, entry->classInfo, true));
    if (!d->provider->initialize())
        return;
    d->component = d->provider->getComponentPtr();
    d->controller = d->provider->getControllerPtr();
    if (d->controller)
    {
        d->componentHandler = Steinberg::owned(
            new AudioComponentHandler(d->pendingRestartFlags));
        d->controller->setComponentHandler(d->componentHandler);
    }
    d->processor = Steinberg::FUnknownPtr<Vst3Processor>(d->component);
    if (!d->component || !d->processor ||
        d->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) != 0 ||
        d->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) != 1 ||
        d->component->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput) != 1 ||
        d->component->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, true) !=
            Steinberg::kResultTrue ||
        d->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true) !=
            Steinberg::kResultTrue ||
        d->processor->canProcessSampleSize(Steinberg::Vst::kSample32) != Steinberg::kResultTrue)
        return;

    d->restoreState(pluginState);
    if (auto midiMapping = Steinberg::FUnknownPtr<Steinberg::Vst::IMidiMapping>(d->controller))
    {
        for (Steinberg::int16 channel = 0; channel <
             static_cast<Steinberg::int16>(d->pitchBendParameterIds.size()); ++channel)
        {
            Steinberg::Vst::ParamID parameterId = Steinberg::Vst::kNoParamId;
            if (midiMapping->getMidiControllerAssignment(
                    0, channel, Steinberg::Vst::kPitchBend, parameterId) !=
                    Steinberg::kResultTrue || parameterId == Steinberg::Vst::kNoParamId)
                continue;
            const bool parameterExists = std::any_of(
                d->descriptor.parameters.cbegin(), d->descriptor.parameters.cend(),
                [parameterId](const Vst3PluginParameter& parameter)
                { return parameter.id == parameterId; });
            if (parameterExists)
                d->pitchBendParameterIds[static_cast<size_t>(channel)] = parameterId;
        }
    }

    d->controlValues.resize(static_cast<size_t>(d->descriptor.parameters.size()));
    d->normalizedValues.resize(d->controlValues.size(), 0.0);
    d->lastSentValues.resize(d->controlValues.size(), std::numeric_limits<double>::quiet_NaN());
    for (qsizetype index = 0; index < d->descriptor.parameters.size(); ++index)
    {
        const Vst3PluginParameter& parameter = d->descriptor.parameters[index];
        double normalized = d->controller
            ? d->controller->getParamNormalized(parameter.id) : 0.0;
        if (!std::isfinite(normalized))
            normalized = normalizedValueForPlain(parameter, parameter.defaultValue);
        float value = index < parameterValues.size()
            ? parameterValues[index] : plainValueForNormalized(parameter, normalized);
        if (!std::isfinite(value))
            value = parameter.defaultValue;
        value = std::clamp(value, parameter.minimum, parameter.maximum);
        if (parameter.toggled)
            value = value >= 0.5f ? 1.0f : 0.0f;
        else if (parameter.integer)
            value = std::round(value);
        d->controlValues[static_cast<size_t>(index)] = value;
        d->normalizedValues[static_cast<size_t>(index)] = normalizedValueForPlain(parameter, value);
    }
    d->prepareParameterQueues();

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(vst3BlockFrames);
    setup.sampleRate = sampleRate;
    if (d->processor->setupProcessing(setup) != Steinberg::kResultTrue ||
        d->component->setActive(true) != Steinberg::kResultTrue)
        return;
    d->active = true;
    d->reportedLatencyFrames = d->processor->getLatencySamples();
    if (d->processor->setProcessing(true) != Steinberg::kResultTrue)
    {
        d->component->setActive(false);
        d->active = false;
        return;
    }
    d->processing = true;
}

Vst3AudioInstrumentInstance::~Vst3AudioInstrumentInstance() = default;

bool Vst3AudioInstrumentInstance::isValid() const
{
    return d != nullptr && d->active && d->processor;
}

bool Vst3AudioInstrumentInstance::hasPendingLatencyRestart() const
{
    return d != nullptr &&
        (d->pendingRestartFlags.load(std::memory_order_acquire) &
         Steinberg::Vst::kLatencyChanged) != 0;
}

bool Vst3AudioInstrumentInstance::serviceLatencyRestart()
{
    return d != nullptr && d->serviceLatencyRestart();
}

qint64 Vst3AudioInstrumentInstance::processingLatencyFrames() const
{
    return isValid() ? static_cast<qint64>(vst3BlockFrames) + d->reportedLatencyFrames : 0;
}

qsizetype Vst3AudioInstrumentInstance::parameterCount() const
{
    return d == nullptr ? 0 : static_cast<qsizetype>(d->controlValues.size());
}

float Vst3AudioInstrumentInstance::parameterValue(qsizetype index) const
{
    if (d == nullptr || index < 0 || index >= static_cast<qsizetype>(d->controlValues.size()))
        return 0.0f;
    return d->controlValues[static_cast<size_t>(index)];
}

void Vst3AudioInstrumentInstance::setParameter(qsizetype index, float value)
{
    if (d == nullptr || !d->controller || index < 0 ||
        index >= static_cast<qsizetype>(d->controlValues.size()))
        return;
    const Vst3PluginParameter& parameter = d->descriptor.parameters[index];
    if (parameter.readOnly)
        return;
    if (!std::isfinite(value))
        value = parameter.defaultValue;
    value = std::clamp(value, parameter.minimum, parameter.maximum);
    if (parameter.toggled)
        value = value >= 0.5f ? 1.0f : 0.0f;
    else if (parameter.integer)
        value = std::round(value);
    d->controlValues[static_cast<size_t>(index)] = value;
    d->normalizedValues[static_cast<size_t>(index)] = normalizedValueForPlain(parameter, value);
}

bool Vst3AudioInstrumentInstance::saveState(QString& pluginState) const
{
    if (!isValid())
        return false;
    Steinberg::ResizableMemoryIBStream componentStream;
    Steinberg::ResizableMemoryIBStream controllerStream;
    const bool componentSaved = d->component->getState(&componentStream) == Steinberg::kResultTrue;
    const bool controllerSaved = d->controller &&
        d->controller->getState(&controllerStream) == Steinberg::kResultTrue;
    if (!componentSaved && !controllerSaved)
        return false;

    const QByteArray componentBytes = streamBytes(componentStream);
    const QByteArray controllerBytes = streamBytes(controllerStream);
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << static_cast<quint32>(componentBytes.size());
    if (!componentBytes.isEmpty())
        stream.writeRawData(componentBytes.constData(), componentBytes.size());
    stream << static_cast<quint32>(controllerBytes.size());
    if (!controllerBytes.isEmpty())
        stream.writeRawData(controllerBytes.constData(), controllerBytes.size());
    const QByteArray encoded = payload.toBase64();
    const QByteArray prefix(vst3StatePrefix);
    if (stream.status() != QDataStream::Ok || encoded.size() + prefix.size() > maximumPluginStateBytes)
        return false;
    pluginState = QString::fromLatin1(prefix + encoded);
    return true;
}

void Vst3AudioInstrumentInstance::stopProcessing()
{
    if (d != nullptr && d->processing && d->processor)
    {
        d->processor->setProcessing(false);
        d->processing = false;
    }
}

void Vst3AudioInstrumentInstance::resetProcessingState()
{
    if (d == nullptr)
        return;
    d->inputEvents.clear();
    d->bufferedFrames = 0;
    d->outputRead = 0;
    d->outputReady = false;
    d->outputLeft.fill(0.0f);
    d->outputRight.fill(0.0f);
    d->monoOutput.fill(0.0f);
    std::fill(d->lastSentValues.begin(), d->lastSentValues.end(),
              std::numeric_limits<double>::quiet_NaN());
    if (d->processing && d->processor)
    {
        d->processor->setProcessing(false);
        d->processing = false;
    }
}

void Vst3AudioInstrumentInstance::allNotesOff()
{
    if (d == nullptr)
        return;
    for (Private::ActiveNote& note : d->activeNotes)
        if (note.active)
            noteOff(note.id, note.channel, note.key);
    for (int channel = 0; channel < 16; ++channel)
        pitchBend(channel, 8192);
}

void Vst3AudioInstrumentInstance::noteOn(std::int32_t noteId, int channel, int key, float velocity)
{
    if (!isValid() || noteId <= 0)
        return;
    Private::ActiveNote* active = nullptr;
    for (Private::ActiveNote& note : d->activeNotes)
    {
        if (note.active && note.id == noteId)
        {
            noteOff(note.id, note.channel, note.key);
            note.active = false;
        }
        if (!note.active && active == nullptr)
            active = &note;
    }
    if (active == nullptr)
        return;
    Steinberg::Vst::Event event{};
    event.type = Steinberg::Vst::Event::kNoteOnEvent;
    event.noteOn.channel = static_cast<Steinberg::int16>(std::clamp(channel, 0, 15));
    event.noteOn.pitch = static_cast<Steinberg::int16>(std::clamp(key, 0, 127));
    event.noteOn.tuning = 0.0f;
    event.noteOn.velocity = std::clamp(velocity, 0.0f, 1.0f);
    event.noteOn.length = 0;
    event.noteOn.noteId = noteId;
    if (!d->pushEvent(event))
        return;
    *active = {noteId, std::clamp(channel, 0, 15), std::clamp(key, 0, 127), true};
}

void Vst3AudioInstrumentInstance::noteOff(std::int32_t noteId, int channel, int key, float velocity)
{
    if (!isValid() || noteId <= 0)
        return;
    Steinberg::Vst::Event event{};
    event.type = Steinberg::Vst::Event::kNoteOffEvent;
    event.noteOff.channel = static_cast<Steinberg::int16>(std::clamp(channel, 0, 15));
    event.noteOff.pitch = static_cast<Steinberg::int16>(std::clamp(key, 0, 127));
    event.noteOff.velocity = std::clamp(velocity, 0.0f, 1.0f);
    event.noteOff.noteId = noteId;
    event.noteOff.tuning = 0.0f;
    if (d->pushEvent(event))
        for (Private::ActiveNote& note : d->activeNotes)
            if (note.active && note.id == noteId)
                note.active = false;
}

void Vst3AudioInstrumentInstance::pitchBend(int channel, int value)
{
    if (d == nullptr || channel < 0 || channel >=
            static_cast<int>(d->pitchBendParameterIds.size()) ||
        d->pitchBendParameterIds[static_cast<size_t>(channel)] == Steinberg::Vst::kNoParamId)
        return;
    const size_t channelIndex = static_cast<size_t>(channel);
    d->pitchBendNormalizedValues[channelIndex] = Steinberg::Vst::midi14BitToNormalized<double>(
        static_cast<std::uint16_t>(std::clamp(value, 0, 16383)));
    d->pitchBendChangeOrder[channelIndex] = ++d->nextPitchBendChangeOrder;
}

void Vst3AudioInstrumentInstance::notePressure(std::int32_t noteId, int channel, int key,
                                               float value)
{
    if (!isValid() || noteId <= 0 || !std::isfinite(value))
        return;
    Steinberg::Vst::Event event{};
    event.type = Steinberg::Vst::Event::kPolyPressureEvent;
    event.polyPressure.channel = static_cast<Steinberg::int16>(std::clamp(channel, 0, 15));
    event.polyPressure.pitch = static_cast<Steinberg::int16>(std::clamp(key, 0, 127));
    event.polyPressure.pressure = std::clamp(value, 0.0f, 1.0f);
    event.polyPressure.noteId = noteId;
    d->pushEvent(event);
}

void Vst3AudioInstrumentInstance::setTransportContext(
    const Vst3TransportContext& transportContext)
{
    if (d != nullptr && d->bufferedFrames == 0)
        updateProcessContext(transportContext, d->processContext);
}

bool Vst3AudioInstrumentInstance::process(
    float& left, float& right, const Vst3TransportContext& transportContext)
{
    if (!isValid())
        return false;
    if (d->bufferedFrames == 0)
        updateProcessContext(transportContext, d->processContext);
    if (!d->processing)
    {
        if (d->processor->setProcessing(true) != Steinberg::kResultTrue)
            return false;
        d->processing = true;
    }
    for (size_t index = 0; index < d->normalizedValues.size(); ++index)
    {
        const Steinberg::Vst::ParamID parameterId =
            d->descriptor.parameters[static_cast<qsizetype>(index)].id;
        double value = d->normalizedValues[index];
        std::uint64_t newestPitchBend = 0;
        for (size_t channelIndex = 0; channelIndex < d->pitchBendParameterIds.size();
             ++channelIndex)
        {
            if (d->pitchBendParameterIds[channelIndex] == parameterId &&
                d->pitchBendChangeOrder[channelIndex] > newestPitchBend)
            {
                newestPitchBend = d->pitchBendChangeOrder[channelIndex];
                value = d->pitchBendNormalizedValues[channelIndex];
            }
        }
        if (std::isfinite(d->lastSentValues[index]) &&
            std::abs(d->lastSentValues[index] - value) <= 1.0e-10)
            continue;
        Steinberg::int32 queueIndex = 0;
        auto* queue = d->parameterChanges.addParameterData(parameterId, queueIndex);
        if (queue == nullptr)
            continue;
        Steinberg::int32 pointIndex = 0;
        queue->addPoint(static_cast<Steinberg::int32>(d->bufferedFrames), value, pointIndex);
        d->lastSentValues[index] = value;
    }

    left = 0.0f;
    right = 0.0f;
    if (d->outputReady)
    {
        left = d->outputChannels == 1 ? d->monoOutput[d->outputRead]
                                      : d->outputLeft[d->outputRead];
        right = d->outputChannels == 1 ? d->monoOutput[d->outputRead]
                                       : d->outputRight[d->outputRead];
        if (++d->outputRead >= vst3BlockFrames)
        {
            d->outputReady = false;
            d->outputRead = 0;
        }
    }
    if (++d->bufferedFrames == vst3BlockFrames)
        d->processBlock();
    return true;
}
