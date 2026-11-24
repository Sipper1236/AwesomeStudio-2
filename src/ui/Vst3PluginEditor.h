#pragma once

#include "core/Vst3AudioEffect.h"

#include <QDialog>

#include <functional>
#include <memory>

class QWidget;

class Vst3PluginEditorWindow final : public QDialog
{
public:
    using ParameterChangeHandler = std::function<void(std::uint32_t, float)>;

    Vst3PluginEditorWindow(const Vst3PluginDescriptor& descriptor,
                           const QString& pluginState,
                           const QVector<float>& parameterValues,
                           ParameterChangeHandler parameterChangeHandler,
                           QWidget* parent = nullptr);
    ~Vst3PluginEditorWindow() override;

    bool openNativeEditor(QString& error);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
