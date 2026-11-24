#include "Vst3PluginEditor.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QEvent>
#include <QHBoxLayout>
#include <QWidget>

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <public.sdk/source/vst/utility/memoryibstream.h>
#include <public.sdk/source/vst/utility/stringconvert.h>
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>

namespace
{
using Vst3Module = VST3::Hosting::Module;
using Vst3ClassInfo = VST3::Hosting::ClassInfo;
using Vst3Factory = VST3::Hosting::PluginFactory;
using Vst3Component = Steinberg::Vst::IComponent;
using Vst3Controller = Steinberg::Vst::IEditController;
using Vst3PlugView = Steinberg::IPlugView;
using Vst3PlugFrame = Steinberg::IPlugFrame;
using Vst3ViewRect = Steinberg::ViewRect;

constexpr int maximumPluginStateBytes = 1024 * 1024;
constexpr auto vst3StatePrefix = "VST3_STATE_V1:";

class EditorHostApplication final : public Steinberg::Vst::HostApplication
{
public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override
    {
        return Steinberg::Vst::StringConvert::convert("Awesome Studio", name)
            ? Steinberg::kResultTrue : Steinberg::kInternalError;
    }
};

class EditorComponentHandler final : public Steinberg::Vst::IComponentHandler
{
public:
    explicit EditorComponentHandler(Vst3Controller* controller,
                                    Vst3PluginEditorWindow::ParameterChangeHandler handler)
        : controller(controller), handler(std::move(handler))
    {
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                  void** object) override
    {
        if (object == nullptr)
            return Steinberg::kInvalidArgument;
        if (Steinberg::FUnknownPrivate::iidEqual(iid,
                                                  Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid))
        {
            *object = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            addRef();
            return Steinberg::kResultTrue;
        }
        *object = nullptr;
        return Steinberg::kNoInterface;
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

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                               Steinberg::Vst::ParamValue normalized) override
    {
        if (!controller || !handler || !std::isfinite(normalized))
            return Steinberg::kResultFalse;
        const double plain = controller->normalizedParamToPlain(id, normalized);
        if (!std::isfinite(plain) || std::abs(plain) > 1.0e9)
            return Steinberg::kResultFalse;
        handler(id, static_cast<float>(plain));
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override
    {
        return Steinberg::kNotImplemented;
    }

private:
    std::atomic<Steinberg::uint32> referenceCount{1};
    Steinberg::IPtr<Vst3Controller> controller;
    Vst3PluginEditorWindow::ParameterChangeHandler handler;
};

class EditorPlugFrame final : public Vst3PlugFrame
{
public:
    using ResizeHandler = std::function<void(int, int)>;

    explicit EditorPlugFrame(ResizeHandler resizeHandler)
        : resizeHandler(std::move(resizeHandler))
    {
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                                  void** object) override
    {
        if (object == nullptr)
            return Steinberg::kInvalidArgument;
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Vst3PlugFrame::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid))
        {
            *object = static_cast<Vst3PlugFrame*>(this);
            addRef();
            return Steinberg::kResultTrue;
        }
        *object = nullptr;
        return Steinberg::kNoInterface;
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

    Steinberg::tresult PLUGIN_API resizeView(Vst3PlugView* view,
                                              Vst3ViewRect* newSize) override
    {
        if (view == nullptr || newSize == nullptr || !resizeHandler ||
            newSize->getWidth() <= 0 || newSize->getHeight() <= 0)
            return Steinberg::kInvalidArgument;
        resizeHandler(newSize->getWidth(), newSize->getHeight());
        return view->onSize(newSize);
    }

private:
    std::atomic<Steinberg::uint32> referenceCount{1};
    ResizeHandler resizeHandler;
};

bool restorePluginState(const QString& pluginState, Vst3Component* component,
                        Vst3Controller* controller)
{
    if (pluginState.isEmpty() || !pluginState.startsWith(QLatin1String(vst3StatePrefix)))
        return true;
    const QByteArray payload = QByteArray::fromBase64(
        pluginState.mid(static_cast<qsizetype>(std::char_traits<char>::length(vst3StatePrefix)))
            .toLatin1());
    if (payload.size() > maximumPluginStateBytes)
        return false;

    QDataStream stream(payload);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 componentSize = 0;
    quint32 controllerSize = 0;
    stream >> componentSize;
    if (stream.status() != QDataStream::Ok || componentSize > maximumPluginStateBytes)
        return false;
    QByteArray componentBytes(static_cast<qsizetype>(componentSize), Qt::Uninitialized);
    if (componentSize > 0 && stream.readRawData(componentBytes.data(),
            static_cast<int>(componentSize)) != static_cast<int>(componentSize))
        return false;
    stream >> controllerSize;
    if (stream.status() != QDataStream::Ok || controllerSize > maximumPluginStateBytes ||
        static_cast<quint64>(componentSize) + controllerSize > maximumPluginStateBytes)
        return false;
    QByteArray controllerBytes(static_cast<qsizetype>(controllerSize), Qt::Uninitialized);
    if (controllerSize > 0 && stream.readRawData(controllerBytes.data(),
            static_cast<int>(controllerSize)) != static_cast<int>(controllerSize))
        return false;
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        return false;

    Steinberg::ResizableMemoryIBStream componentStream(componentBytes.size());
    Steinberg::ResizableMemoryIBStream controllerStream(controllerBytes.size());
    const auto load = [](const QByteArray& bytes, Steinberg::ResizableMemoryIBStream& target)
    {
        return bytes.isEmpty() || target.write(const_cast<char*>(bytes.constData()),
            static_cast<Steinberg::int32>(bytes.size()), nullptr) == Steinberg::kResultTrue;
    };
    if (!load(componentBytes, componentStream) || !load(controllerBytes, controllerStream))
        return false;
    if (!componentBytes.isEmpty())
    {
        component->setState(&componentStream);
        componentStream.rewind();
        if (controller)
            controller->setComponentState(&componentStream);
    }
    if (controller && !controllerBytes.isEmpty())
        controller->setState(&controllerStream);
    return true;
}
} // namespace

struct Vst3PluginEditorWindow::Private
{
    Vst3PluginDescriptor descriptor;
    Vst3Module::Ptr module;
    Steinberg::IPtr<Steinberg::Vst::PlugProvider> provider;
    Steinberg::IPtr<Vst3Component> component;
    Steinberg::IPtr<Vst3Controller> controller;
    Steinberg::IPtr<Vst3PlugView> plugView;
    Steinberg::IPtr<EditorComponentHandler> componentHandler;
    Steinberg::IPtr<EditorPlugFrame> plugFrame;
    ParameterChangeHandler parameterChangeHandler;
    QWidget* viewContainer = nullptr;
    Vst3ViewRect viewSize{};
    bool attached = false;

    ~Private()
    {
        detach();
        if (controller)
            controller->setComponentHandler(nullptr);
    }

    bool prepare(const QString& pluginState, const QVector<float>& parameterValues,
                 QString& error)
    {
        std::string moduleError;
        module = Vst3Module::create(descriptor.path.toStdString(), moduleError);
        if (!module)
        {
            error = QString::fromStdString(moduleError);
            return false;
        }
        const Vst3Factory factory = module->getFactory();
        const auto classInfos = factory.classInfos();
        const auto classInfo = std::find_if(classInfos.cbegin(), classInfos.cend(),
            [this](const Vst3ClassInfo& info)
            { return QString::fromStdString(info.ID().toString(false)) == descriptor.id; });
        if (classInfo == classInfos.cend())
        {
            error = QObject::tr("The VST3 class is no longer available in this bundle.");
            return false;
        }

        Steinberg::FUnknown* previousContext =
            Steinberg::Vst::PluginContextFactory::instance().getPluginContext();
        static EditorHostApplication editorHost;
        Steinberg::Vst::PluginContextFactory::instance().setPluginContext(&editorHost);
        Vst3Factory editorFactory = factory;
        editorFactory.setHostContext(&editorHost);
        provider = Steinberg::owned(new Steinberg::Vst::PlugProvider(
            editorFactory, *classInfo, true));
        const bool initialized = provider->initialize();
        Steinberg::Vst::PluginContextFactory::instance().setPluginContext(previousContext);
        if (!initialized)
        {
            error = QObject::tr("The VST3 editor instance could not be initialized.");
            return false;
        }
        component = provider->getComponentPtr();
        controller = provider->getControllerPtr();
        if (!component || !controller)
        {
            error = QObject::tr("This VST3 plug-in does not provide an editor controller.");
            return false;
        }

        if (!restorePluginState(pluginState, component, controller))
        {
            error = QObject::tr("The saved VST3 instrument state could not be read.");
            return false;
        }
        const Steinberg::int32 count = std::min<Steinberg::int32>(
            static_cast<Steinberg::int32>(descriptor.parameters.size()),
            static_cast<Steinberg::int32>(parameterValues.size()));
        for (Steinberg::int32 index = 0; index < count; ++index)
        {
            const Vst3PluginParameter& parameter = descriptor.parameters[index];
            const float plain = std::clamp(parameterValues[index],
                                           parameter.minimum, parameter.maximum);
            const double normalized = controller->plainParamToNormalized(parameter.id, plain);
            if (std::isfinite(normalized))
                controller->setParamNormalized(parameter.id, std::clamp(normalized, 0.0, 1.0));
        }

        componentHandler = Steinberg::owned(new EditorComponentHandler(
            controller, [this](std::uint32_t id, float value)
            {
                if (parameterChangeHandler)
                    parameterChangeHandler(id, value);
            }));
        controller->setComponentHandler(componentHandler);
        plugView = Steinberg::owned(controller->createView(Steinberg::Vst::ViewType::kEditor));
        if (!plugView)
        {
            error = QObject::tr("This VST3 plug-in does not provide a native editor.");
            return false;
        }
        if (plugView->getSize(&viewSize) != Steinberg::kResultTrue ||
            viewSize.getWidth() <= 0 || viewSize.getHeight() <= 0)
        {
            error = QObject::tr("The VST3 editor did not provide a valid window size.");
            return false;
        }
        return true;
    }

    void detach()
    {
        if (plugView && attached)
        {
            plugView->setFrame(nullptr);
            plugView->removed();
            attached = false;
        }
        plugView = nullptr;
        plugFrame = nullptr;
        viewContainer = nullptr;
    }
};

Vst3PluginEditorWindow::Vst3PluginEditorWindow(
    const Vst3PluginDescriptor& descriptor, const QString& pluginState,
    const QVector<float>& parameterValues, ParameterChangeHandler parameterChangeHandler,
    QWidget* parent)
    : QDialog(parent), d(std::make_unique<Private>())
{
    d->descriptor = descriptor;
    d->parameterChangeHandler = std::move(parameterChangeHandler);
    setWindowTitle(descriptor.name);
    setWindowFlag(Qt::Window, true);
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("vst3NativeEditorWindow", true);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* container = new QWidget(this);
    container->setAttribute(Qt::WA_NativeWindow);
    container->setAutoFillBackground(false);
    layout->addWidget(container);
    d->viewContainer = container;

    QString error;
    if (!d->prepare(pluginState, parameterValues, error))
        setProperty("vst3EditorError", error);
}

Vst3PluginEditorWindow::~Vst3PluginEditorWindow() = default;

bool Vst3PluginEditorWindow::openNativeEditor(QString& error)
{
    if (!property("vst3EditorError").toString().isEmpty())
    {
        error = property("vst3EditorError").toString();
        return false;
    }
    if (!d->plugView || d->viewContainer == nullptr)
    {
        error = tr("The VST3 native editor could not be created.");
        return false;
    }

    const qreal scale = d->viewContainer->devicePixelRatioF();
    const qreal coordinateScale = qApp->platformName() == QLatin1String("cocoa")
        ? 1.0 : std::max<qreal>(1.0, scale);
    d->viewContainer->setFixedSize(
        static_cast<int>(std::ceil(d->viewSize.getWidth() / coordinateScale)),
        static_cast<int>(std::ceil(d->viewSize.getHeight() / coordinateScale)));
    adjustSize();
    show();
    QCoreApplication::processEvents();

    const QString platform = qApp->platformName();
    const char* platformType = nullptr;
    void* nativeHandle = nullptr;
#if defined(Q_OS_WIN)
    platformType = Steinberg::kPlatformTypeHWND;
    nativeHandle = reinterpret_cast<void*>(d->viewContainer->winId());
#elif defined(Q_OS_MACOS)
    platformType = Steinberg::kPlatformTypeNSView;
    nativeHandle = reinterpret_cast<void*>(d->viewContainer->winId());
#elif defined(Q_OS_LINUX)
    if (!platform.startsWith(QLatin1String("xcb")))
    {
        error = tr("Native VST3 editor embedding on Linux currently requires the X11 platform. Generic instrument controls remain available on Wayland.");
        hide();
        return false;
    }
    platformType = Steinberg::kPlatformTypeX11EmbedWindowID;
    nativeHandle = reinterpret_cast<void*>(static_cast<quintptr>(d->viewContainer->winId()));
#else
    Q_UNUSED(platform)
    error = tr("Native VST3 editor embedding is not supported on this platform.");
    hide();
    return false;
#endif

    if (nativeHandle == nullptr || d->plugView->isPlatformTypeSupported(platformType) !=
                                      Steinberg::kResultTrue)
    {
        error = tr("This VST3 editor does not support the current window platform.");
        hide();
        return false;
    }

    const auto resizeView = [this, coordinateScale](int width, int height)
    {
        d->viewSize.right = d->viewSize.left + width;
        d->viewSize.bottom = d->viewSize.top + height;
        d->viewContainer->setFixedSize(
            static_cast<int>(std::ceil(width / coordinateScale)),
            static_cast<int>(std::ceil(height / coordinateScale)));
        adjustSize();
    };
    d->plugFrame = Steinberg::owned(new EditorPlugFrame(resizeView));
    if (auto scaleSupport = Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(
            d->plugView))
        scaleSupport->setContentScaleFactor(static_cast<Steinberg::IPlugViewContentScaleSupport::ScaleFactor>(scale));
    d->plugView->setFrame(d->plugFrame);
    if (d->plugView->attached(nativeHandle, platformType) != Steinberg::kResultTrue)
    {
        d->plugView->setFrame(nullptr);
        d->plugFrame = nullptr;
        error = tr("The VST3 editor refused the host window.");
        hide();
        return false;
    }
    d->attached = true;
    return true;
}
