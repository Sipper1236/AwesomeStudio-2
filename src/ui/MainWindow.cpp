#include "MainWindow.h"

#include "core/AudioFile.h"
#include "core/AudioRecorder.h"
#include "core/ClapAudioEffect.h"
#include "core/ProjectFile.h"
#include "core/MidiFile.h"
#include "core/MidiInput.h"
#include "core/WavExporter.h"
#include "ui/ArrangementEditor.h"
#include "ui/MidiEditor.h"
#include "ui/StepSequencer.h"

#include <QAction>
#include <QApplication>
#include <QAudioDevice>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMediaDevices>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QStringList>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
constexpr double automationWritePointSpacingBeats = 0.04;

class TrackEqResponseWidget final : public QWidget
{
public:
    using ChangeHandler = std::function<void(int, float, float)>;
    using SelectionHandler = std::function<void(int)>;

    explicit TrackEqResponseWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(190, 148);
        setMaximumHeight(170);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setAccessibleName(tr("Parametric EQ response"));
        setToolTip(tr("Drag a band point to change its frequency and gain"));
    }

    void setBands(const std::array<ParametricEqBand, 3>& bands)
    {
        parametricBands = bands;
        update();
    }

    void setLegacyParameters(float lowDb, float midDb, float highDb,
                             float lowCrossoverHz, float highCrossoverHz)
    {
        legacyGains = {lowDb, midDb, highDb};
        legacyCrossovers = {lowCrossoverHz, highCrossoverHz};
        update();
    }

    void setSelectedBand(int band)
    {
        selectedBand = std::clamp(band, 0, 2);
        update();
    }

    void setChangeHandler(ChangeHandler handler) { changeHandler = std::move(handler); }
    void setSelectionHandler(SelectionHandler handler) { selectionHandler = std::move(handler); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor background = palette().color(QPalette::Base);
        const QColor foreground = palette().color(QPalette::Text);
        const QColor gridColor = palette().color(QPalette::Mid);
        const QColor accent = palette().color(QPalette::Highlight);
        painter.fillRect(rect(), background);

        const QRectF plot = plotRect();
        if (plot.width() < 40.0 || plot.height() < 40.0)
            return;
        painter.setPen(QPen(gridColor, 1.0));
        painter.drawRect(plot);

        const QFontMetrics metrics(painter.font());
        painter.setPen(foreground);
        for (int db = -18; db <= 18; db += 6)
        {
            const double y = gainToY(db, plot);
            painter.setPen(QPen(gridColor, db == 0 ? 1.0 : 0.7,
                                db == 0 ? Qt::SolidLine : Qt::DotLine));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(foreground);
            const QString label = db > 0 ? QStringLiteral("+%1").arg(db)
                                         : QString::number(db);
            painter.drawText(QRectF(0.0, y - metrics.height() / 2.0,
                                    plot.left() - 5.0, metrics.height()),
                             Qt::AlignRight | Qt::AlignVCenter, label);
        }

        const std::array<double, 5> frequencies = {20.0, 100.0, 1000.0, 10000.0, 20000.0};
        for (double frequency : frequencies)
        {
            const double x = frequencyToX(frequency, plot);
            painter.setPen(QPen(gridColor, 0.7, Qt::DotLine));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.setPen(foreground);
            const QString label = frequency >= 1000.0
                ? QStringLiteral("%1k").arg(frequency / 1000.0, 0, 'g', 2)
                : QString::number(static_cast<int>(frequency));
            const double labelX = std::clamp(x - 20.0, plot.left(), plot.right() - 40.0);
            painter.drawText(QRectF(labelX, plot.bottom() + 3.0, 40.0, metrics.height()),
                             Qt::AlignHCenter | Qt::AlignTop, label);
        }

        QPainterPath response;
        constexpr int pointCount = 180;
        for (int index = 0; index <= pointCount; ++index)
        {
            const double ratio = static_cast<double>(index) / pointCount;
            const double frequency = 20.0 * std::pow(1000.0, ratio);
            const double gainDb = std::clamp(responseAt(frequency), -18.0, 18.0);
            const QPointF point(frequencyToX(frequency, plot), gainToY(gainDb, plot));
            if (index == 0)
                response.moveTo(point);
            else
                response.lineTo(point);
        }
        painter.setPen(QPen(accent, 2.0));
        painter.drawPath(response);

        for (int bandIndex = 0; bandIndex < static_cast<int>(parametricBands.size()); ++bandIndex)
        {
            const ParametricEqBand& band = parametricBands[static_cast<size_t>(bandIndex)];
            const QPointF center(frequencyToX(band.frequencyHz, plot),
                                 gainToY(band.gainDb, plot));
            const QColor color = bandIndex == selectedBand ? accent : foreground;
            painter.setPen(QPen(background, 2.0));
            painter.setBrush(color);
            painter.drawEllipse(center, bandIndex == selectedBand ? 6.0 : 4.5,
                                bandIndex == selectedBand ? 6.0 : 4.5);
            painter.setPen(QPen(background, 1.0));
            painter.drawText(QRectF(center.x() - 5.0, center.y() - 5.0, 10.0, 10.0),
                             Qt::AlignCenter, QString::number(bandIndex + 1));
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !plotRect().contains(event->position()))
            return;
        const QPointF position = event->position();
        int closestBand = selectedBand;
        double closestDistance = 14.0;
        const QRectF plot = plotRect();
        for (int bandIndex = 0; bandIndex < static_cast<int>(parametricBands.size()); ++bandIndex)
        {
            const ParametricEqBand& band = parametricBands[static_cast<size_t>(bandIndex)];
            const QPointF center(frequencyToX(band.frequencyHz, plot),
                                 gainToY(band.gainDb, plot));
            const double distance = std::hypot(position.x() - center.x(), position.y() - center.y());
            if (distance < closestDistance)
            {
                closestDistance = distance;
                closestBand = bandIndex;
            }
        }
        selectedBand = closestBand;
        dragging = true;
        setSelectedBand(selectedBand);
        if (selectionHandler)
            selectionHandler(selectedBand);
        updateFromPosition(position);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (dragging)
        {
            updateFromPosition(event->position());
            event->accept();
            return;
        }
        const QPointF position = event->position();
        bool nearHandle = false;
        const QRectF plot = plotRect();
        for (const ParametricEqBand& band : parametricBands)
        {
            const QPointF center(frequencyToX(band.frequencyHz, plot),
                                 gainToY(band.gainDb, plot));
            if (std::hypot(position.x() - center.x(), position.y() - center.y()) < 11.0)
            {
                nearHandle = true;
                break;
            }
        }
        setCursor(nearHandle ? Qt::OpenHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            dragging = false;
            unsetCursor();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QRectF plotRect() const
    {
        return QRectF(37.0, 7.0, std::max(1, width() - 46), std::max(1, height() - 32));
    }

    static double frequencyToX(double frequency, const QRectF& plot)
    {
        const double normalized = std::log(std::clamp(frequency, 20.0, 20000.0) / 20.0) /
                                  std::log(1000.0);
        return plot.left() + normalized * plot.width();
    }

    static double gainToY(double gainDb, const QRectF& plot)
    {
        return plot.top() + (18.0 - std::clamp(gainDb, -18.0, 18.0)) / 36.0 * plot.height();
    }

    double responseAt(double frequency) const
    {
        constexpr double sampleRate = 48000.0;
        constexpr double twoPi = 6.28318530717958647692;
        const double omega = twoPi * frequency / sampleRate;
        const std::complex<double> z1(std::cos(omega), -std::sin(omega));
        const std::complex<double> z2 = z1 * z1;

        const auto lowpass = [omega, sampleRate, twoPi](double cutoff)
        {
            const double coefficient = 1.0 - std::exp(-twoPi * cutoff / sampleRate);
            const std::complex<double> z(std::cos(omega), -std::sin(omega));
            return coefficient / (1.0 - (1.0 - coefficient) * z);
        };
        const std::complex<double> low = lowpass(legacyCrossovers[0]);
        const std::complex<double> upper = lowpass(legacyCrossovers[1]);
        std::complex<double> response =
            std::pow(10.0, legacyGains[0] / 20.0) * low +
            std::pow(10.0, legacyGains[1] / 20.0) * (upper - low) +
            std::pow(10.0, legacyGains[2] / 20.0) * (1.0 - upper);

        for (const ParametricEqBand& band : parametricBands)
        {
            if (std::abs(band.gainDb) < 1.0e-5f)
                continue;
            const double center = twoPi * std::clamp(static_cast<double>(band.frequencyHz),
                20.0, sampleRate * 0.49) / sampleRate;
            const double q = std::clamp(static_cast<double>(band.q), 0.1, 10.0);
            const double a = std::pow(10.0, band.gainDb / 40.0);
            const double alpha = std::sin(center) / (2.0 * q);
            const double cosine = std::cos(center);
            const double a0 = 1.0 + alpha / a;
            const double b0 = (1.0 + alpha * a) / a0;
            const double b1 = (-2.0 * cosine) / a0;
            const double b2 = (1.0 - alpha * a) / a0;
            const double a1 = (-2.0 * cosine) / a0;
            const double a2 = (1.0 - alpha / a) / a0;
            const std::complex<double> numerator = b0 + b1 * z1 + b2 * z2;
            const std::complex<double> denominator = 1.0 + a1 * z1 + a2 * z2;
            response *= numerator / denominator;
        }
        return 20.0 * std::log10(std::max(1.0e-6, std::abs(response)));
    }

    void updateFromPosition(const QPointF& position)
    {
        const QRectF plot = plotRect();
        if (!plot.contains(position) || !changeHandler)
            return;
        const double ratio = std::clamp((position.x() - plot.left()) / plot.width(), 0.0, 1.0);
        const float frequency = static_cast<float>(20.0 * std::pow(1000.0, ratio));
        const float gainDb = static_cast<float>(std::clamp(
            18.0 - (position.y() - plot.top()) / plot.height() * 36.0, -18.0, 18.0));
        const ParametricEqBand& current = parametricBands[static_cast<size_t>(selectedBand)];
        if (std::abs(current.frequencyHz - frequency) < 0.2f &&
            std::abs(current.gainDb - gainDb) < 0.02f)
            return;
        changeHandler(selectedBand, frequency, gainDb);
    }

    std::array<ParametricEqBand, 3> parametricBands{{
        {120.0f, 0.0f, 0.707f}, {1000.0f, 0.0f, 0.707f}, {8000.0f, 0.0f, 0.707f}
    }};
    std::array<float, 3> legacyGains{};
    std::array<float, 2> legacyCrossovers{120.0f, 6000.0f};
    ChangeHandler changeHandler;
    SelectionHandler selectionHandler;
    int selectedBand = 0;
    bool dragging = false;
};

struct FactorySynthPreset
{
    const char* name;
    SynthWaveform waveform;
    bool secondOscillatorEnabled;
    SynthWaveform secondOscillatorWaveform;
    float secondOscillatorDetuneCents;
    float secondOscillatorMix;
    float attackSeconds;
    float decaySeconds;
    float sustainLevel;
    float releaseSeconds;
    bool filterEnabled;
    SynthFilterType filterType;
    float filterCutoffHz;
    float filterResonance;
    bool filterEnvelopeEnabled;
    float filterEnvelopeAmountSemitones;
    float filterEnvelopeAttackSeconds;
    float filterEnvelopeDecaySeconds;
    float filterEnvelopeSustainLevel;
    float filterEnvelopeReleaseSeconds;
};

constexpr std::array<FactorySynthPreset, 7> factorySynthPresets = {{
    {"Deep Sub", SynthWaveform::Sine, false, SynthWaveform::Saw, 0.0f, 0.5f,
     0.008f, 0.08f, 0.85f, 0.1f, true, SynthFilterType::LowPass, 360.0f, 0.707f,
     false, 24.0f, 0.008f, 0.2f, 0.0f, 0.2f},
    {"Rubber Bass", SynthWaveform::Triangle, true, SynthWaveform::Square, -7.0f, 0.22f,
     0.004f, 0.18f, 0.62f, 0.1f, true, SynthFilterType::LowPass, 900.0f, 1.4f,
     true, -16.0f, 0.005f, 0.15f, 0.18f, 0.12f},
    {"Bright Lead", SynthWaveform::Saw, true, SynthWaveform::Square, 7.0f, 0.32f,
     0.006f, 0.13f, 0.72f, 0.15f, true, SynthFilterType::LowPass, 5200.0f, 0.8f,
     true, 18.0f, 0.012f, 0.2f, 0.32f, 0.14f},
    {"Warm Pad", SynthWaveform::Triangle, true, SynthWaveform::Sine, -5.0f, 0.3f,
     0.55f, 0.9f, 0.76f, 1.3f, true, SynthFilterType::LowPass, 2300.0f, 0.71f,
     true, 9.0f, 0.5f, 0.8f, 0.3f, 0.8f},
    {"Glass Keys", SynthWaveform::Sine, true, SynthWaveform::Triangle, 3.0f, 0.22f,
     0.002f, 0.4f, 0.18f, 0.55f, true, SynthFilterType::LowPass, 8500.0f, 0.72f,
     true, -6.0f, 0.001f, 0.4f, 0.05f, 0.4f},
    {"Pluck", SynthWaveform::Saw, true, SynthWaveform::Triangle, 11.0f, 0.18f,
     0.001f, 0.3f, 0.04f, 0.18f, true, SynthFilterType::LowPass, 1900.0f, 1.2f,
     true, 30.0f, 0.001f, 0.25f, 0.0f, 0.15f},
    {"Organ", SynthWaveform::Square, true, SynthWaveform::Sine, 0.0f, 0.28f,
     0.005f, 0.05f, 0.95f, 0.08f, false, SynthFilterType::LowPass, 20000.0f, 0.707f,
     false, 24.0f, 0.008f, 0.2f, 0.0f, 0.2f}
}};

bool samePresetFloat(float left, float right)
{
    return std::abs(left - right) < 0.001f;
}

bool matchesFactorySynthPreset(const InstrumentTrack& track, const FactorySynthPreset& preset)
{
    return track.waveform == preset.waveform &&
        track.secondOscillatorEnabled == preset.secondOscillatorEnabled &&
        track.secondOscillatorWaveform == preset.secondOscillatorWaveform &&
        samePresetFloat(track.secondOscillatorDetuneCents, preset.secondOscillatorDetuneCents) &&
        samePresetFloat(track.secondOscillatorMix, preset.secondOscillatorMix) &&
        samePresetFloat(track.attackSeconds, preset.attackSeconds) &&
        samePresetFloat(track.decaySeconds, preset.decaySeconds) &&
        samePresetFloat(track.sustainLevel, preset.sustainLevel) &&
        samePresetFloat(track.releaseSeconds, preset.releaseSeconds) &&
        track.synthFilterEnabled == preset.filterEnabled &&
        track.synthFilterType == preset.filterType &&
        samePresetFloat(track.synthFilterCutoffHz, preset.filterCutoffHz) &&
        samePresetFloat(track.synthFilterResonance, preset.filterResonance) &&
        track.synthFilterEnvelopeEnabled == preset.filterEnvelopeEnabled &&
        samePresetFloat(track.synthFilterEnvelopeAmountSemitones,
                        preset.filterEnvelopeAmountSemitones) &&
        samePresetFloat(track.synthFilterEnvelopeAttackSeconds,
                        preset.filterEnvelopeAttackSeconds) &&
        samePresetFloat(track.synthFilterEnvelopeDecaySeconds,
                        preset.filterEnvelopeDecaySeconds) &&
        samePresetFloat(track.synthFilterEnvelopeSustainLevel,
                        preset.filterEnvelopeSustainLevel) &&
        samePresetFloat(track.synthFilterEnvelopeReleaseSeconds,
                        preset.filterEnvelopeReleaseSeconds);
}

void applyFactorySynthPreset(InstrumentTrack& track, const FactorySynthPreset& preset)
{
    track.waveform = preset.waveform;
    track.secondOscillatorEnabled = preset.secondOscillatorEnabled;
    track.secondOscillatorWaveform = preset.secondOscillatorWaveform;
    track.secondOscillatorDetuneCents = preset.secondOscillatorDetuneCents;
    track.secondOscillatorMix = preset.secondOscillatorMix;
    track.attackSeconds = preset.attackSeconds;
    track.decaySeconds = preset.decaySeconds;
    track.sustainLevel = preset.sustainLevel;
    track.releaseSeconds = preset.releaseSeconds;
    track.synthFilterEnabled = preset.filterEnabled;
    track.synthFilterType = preset.filterType;
    track.synthFilterCutoffHz = preset.filterCutoffHz;
    track.synthFilterResonance = preset.filterResonance;
    track.synthFilterEnvelopeEnabled = preset.filterEnvelopeEnabled;
    track.synthFilterEnvelopeAmountSemitones = preset.filterEnvelopeAmountSemitones;
    track.synthFilterEnvelopeAttackSeconds = preset.filterEnvelopeAttackSeconds;
    track.synthFilterEnvelopeDecaySeconds = preset.filterEnvelopeDecaySeconds;
    track.synthFilterEnvelopeSustainLevel = preset.filterEnvelopeSustainLevel;
    track.synthFilterEnvelopeReleaseSeconds = preset.filterEnvelopeReleaseSeconds;
}

float automationValueAt(const QVector<AutomationPoint>& points, double beat, float defaultValue)
{
    if (points.isEmpty())
        return defaultValue;
    const auto next = std::upper_bound(points.cbegin(), points.cend(), beat,
        [](double value, const AutomationPoint& point) { return value < point.beat; });
    if (next == points.cbegin())
        return defaultValue;
    const auto previous = std::prev(next);
    if (next == points.cend())
        return previous->value;
    const double span = next->beat - previous->beat;
    if (span <= 0.0)
        return next->value;
    const float amount = static_cast<float>(automationCurveProgress(
        previous->curveToNext, (beat - previous->beat) / span));
    return previous->value + (next->value - previous->value) * amount;
}

QString automationCurveName(AutomationPoint::Curve curve)
{
    switch (curve)
    {
        case AutomationPoint::Curve::Smooth: return QObject::tr("Smooth");
        case AutomationPoint::Curve::EaseIn: return QObject::tr("Ease In");
        case AutomationPoint::Curve::EaseOut: return QObject::tr("Ease Out");
        case AutomationPoint::Curve::Hold: return QObject::tr("Hold");
        case AutomationPoint::Curve::Linear:
        default: return QObject::tr("Linear");
    }
}

constexpr int pluginAutomationTargetBase = 2;
constexpr int pluginParametersPerEffect = 256;
constexpr int maximumPluginEffectCount = 8;
constexpr int clapInstrumentAutomationTargetBase =
    pluginAutomationTargetBase + pluginParametersPerEffect * maximumPluginEffectCount;

int pluginAutomationTarget(qsizetype effectIndex, qsizetype parameterIndex)
{
    return pluginAutomationTargetBase + static_cast<int>(effectIndex) * pluginParametersPerEffect +
           static_cast<int>(parameterIndex);
}

int clapInstrumentAutomationTarget(qsizetype parameterIndex)
{
    return clapInstrumentAutomationTargetBase + static_cast<int>(parameterIndex);
}

bool decodeClapInstrumentAutomationTarget(int target, qsizetype& parameterIndex)
{
    const int encoded = target - clapInstrumentAutomationTargetBase;
    if (encoded < 0 || encoded >= pluginParametersPerEffect)
        return false;
    parameterIndex = encoded;
    return true;
}

bool decodePluginAutomationTarget(int target, qsizetype& effectIndex, qsizetype& parameterIndex)
{
    if (target < pluginAutomationTargetBase)
        return false;
    const int encoded = target - pluginAutomationTargetBase;
    if (encoded >= pluginParametersPerEffect * maximumPluginEffectCount)
        return false;
    effectIndex = encoded / pluginParametersPerEffect;
    parameterIndex = encoded % pluginParametersPerEffect;
    return true;
}

const QVector<AutomationPoint>* automationCurve(const InstrumentTrack& track, int target,
                                                const QString& pluginUri = QString())
{
    if (target == 0)
        return &track.volumeAutomation;
    if (target == 1)
        return &track.panAutomation;
    qsizetype instrumentParameterIndex = 0;
    if (decodeClapInstrumentAutomationTarget(target, instrumentParameterIndex))
    {
        if (track.instrumentType != InstrumentType::ClapInstrument || pluginUri.isEmpty() ||
            track.clapPluginId != pluginUri)
            return nullptr;
        static const QVector<AutomationPoint> empty;
        if (instrumentParameterIndex >= track.clapParameterAutomation.size())
            return &empty;
        return &track.clapParameterAutomation[instrumentParameterIndex];
    }
    qsizetype effectIndex = 0;
    qsizetype parameterIndex = 0;
    if (!decodePluginAutomationTarget(target, effectIndex, parameterIndex) ||
        effectIndex >= track.effects.size())
        return nullptr;
    const MixerEffect& effect = track.effects[effectIndex];
    if (!isPluginEffect(effect.type) || pluginUri.isEmpty() || effect.pluginUri != pluginUri)
        return nullptr;
    static const QVector<AutomationPoint> empty;
    if (parameterIndex >= effect.pluginParameterAutomation.size())
        return &empty;
    return &effect.pluginParameterAutomation[parameterIndex];
}

QVector<AutomationPoint>* automationCurve(InstrumentTrack& track, int target,
                                          const QString& pluginUri = QString())
{
    if (target == 0)
        return &track.volumeAutomation;
    if (target == 1)
        return &track.panAutomation;
    qsizetype instrumentParameterIndex = 0;
    if (decodeClapInstrumentAutomationTarget(target, instrumentParameterIndex))
    {
        if (track.instrumentType != InstrumentType::ClapInstrument || pluginUri.isEmpty() ||
            track.clapPluginId != pluginUri)
            return nullptr;
        while (track.clapParameterAutomation.size() <= instrumentParameterIndex)
            track.clapParameterAutomation.push_back({});
        return &track.clapParameterAutomation[instrumentParameterIndex];
    }
    qsizetype effectIndex = 0;
    qsizetype parameterIndex = 0;
    if (!decodePluginAutomationTarget(target, effectIndex, parameterIndex) ||
        effectIndex >= track.effects.size())
        return nullptr;
    MixerEffect& effect = track.effects[effectIndex];
    if (!isPluginEffect(effect.type) || pluginUri.isEmpty() || effect.pluginUri != pluginUri)
        return nullptr;
    while (effect.pluginParameterAutomation.size() <= parameterIndex)
        effect.pluginParameterAutomation.push_back({});
    return &effect.pluginParameterAutomation[parameterIndex];
}

void populateAutomationTargets(QComboBox* control, const InstrumentTrack* track)
{
    if (control == nullptr)
        return;
    const int previousTarget = control->currentData().toInt();
    const QString previousPluginUri = control->currentData(Qt::UserRole + 4).toString();
    const QSignalBlocker blocker(control);
    control->clear();
    const auto addTarget = [control](const QString& label, int target, float minimum,
                                     float maximum, float defaultValue, const QString& pluginUri)
    {
        const int index = control->count();
        control->addItem(label, target);
        control->setItemData(index, minimum, Qt::UserRole + 1);
        control->setItemData(index, maximum, Qt::UserRole + 2);
        control->setItemData(index, defaultValue, Qt::UserRole + 3);
        control->setItemData(index, pluginUri, Qt::UserRole + 4);
    };
    addTarget(QObject::tr("Volume"), 0, 0.0f, 1.0f,
              track == nullptr ? 0.8f : track->gain, QString());
    addTarget(QObject::tr("Pan"), 1, -1.0f, 1.0f,
              track == nullptr ? 0.0f : track->pan, QString());
    if (track != nullptr)
    {
        for (qsizetype effectIndex = 0; effectIndex < track->effects.size(); ++effectIndex)
        {
            const MixerEffect& effect = track->effects[effectIndex];
            if (!isPluginEffect(effect.type))
                continue;
            QVector<Lv2EffectParameter> parameters;
            QString discoveredName;
            if (effect.type == MixerEffectType::Lv2Plugin)
            {
                Lv2EffectDescriptor descriptor;
                if (!Lv2AudioEffectInstance::describe(effect.pluginUri, descriptor))
                    continue;
                parameters = descriptor.parameters;
                discoveredName = descriptor.name;
            }
            else
            {
                ClapPluginDescriptor descriptor;
                if (!ClapAudioPluginInstance::describe(
                        effect.pluginUri, ClapPluginKind::AudioEffect, descriptor))
                    continue;
                discoveredName = descriptor.name;
                for (const ClapPluginParameter& parameter : descriptor.parameters)
                {
                    Lv2EffectParameter converted;
                    converted.symbol = QString::number(parameter.id);
                    converted.name = parameter.name;
                    converted.minimum = parameter.minimum;
                    converted.maximum = parameter.maximum;
                    converted.defaultValue = parameter.defaultValue;
                    converted.integer = parameter.integer;
                    converted.toggled = parameter.toggled;
                    converted.hidden = parameter.hidden;
                    converted.readOnly = parameter.readOnly;
                    parameters.push_back(std::move(converted));
                }
            }
            const QString pluginName = effect.pluginName.isEmpty() ? discoveredName : effect.pluginName;
            for (qsizetype parameterIndex = 0;
                 parameterIndex < parameters.size() && parameterIndex < pluginParametersPerEffect;
                 ++parameterIndex)
            {
                const Lv2EffectParameter& parameter = parameters[parameterIndex];
                if (parameter.hidden || parameter.readOnly)
                    continue;
                const float defaultValue = parameterIndex < effect.pluginParameterValues.size()
                    ? effect.pluginParameterValues[parameterIndex] : parameter.defaultValue;
                addTarget(QObject::tr("%1 · %2 · %3")
                              .arg(effectIndex + 1).arg(pluginName, parameter.name),
                          pluginAutomationTarget(effectIndex, parameterIndex),
                          parameter.minimum, parameter.maximum, defaultValue, effect.pluginUri);
            }
        }
        if (track->instrumentType == InstrumentType::ClapInstrument &&
            !track->clapPluginId.isEmpty())
        {
            ClapPluginDescriptor descriptor;
            if (ClapAudioPluginInstance::describe(track->clapPluginId,
                                                  ClapPluginKind::Instrument, descriptor))
            {
                const QString pluginName = track->clapPluginName.isEmpty()
                    ? descriptor.name : track->clapPluginName;
                for (qsizetype parameterIndex = 0;
                     parameterIndex < descriptor.parameters.size() &&
                         parameterIndex < pluginParametersPerEffect;
                     ++parameterIndex)
                {
                    const ClapPluginParameter& parameter = descriptor.parameters[parameterIndex];
                    if (parameter.hidden || parameter.readOnly)
                        continue;
                    const float defaultValue = parameterIndex < track->clapParameterValues.size()
                        ? track->clapParameterValues[parameterIndex] : parameter.defaultValue;
                    addTarget(QObject::tr("Instrument · %1 · %2")
                                  .arg(pluginName, parameter.name),
                              clapInstrumentAutomationTarget(parameterIndex),
                              parameter.minimum, parameter.maximum, defaultValue,
                              track->clapPluginId);
                }
            }
        }
    }
    int previousIndex = -1;
    for (int index = 0; index < control->count(); ++index)
    {
        if (control->itemData(index).toInt() == previousTarget &&
            control->itemData(index, Qt::UserRole + 4).toString() == previousPluginUri)
        {
            previousIndex = index;
            break;
        }
    }
    control->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
}

QString projectAutosavePath(const QString& projectPath)
{
    return projectPath + QStringLiteral(".autosave");
}

QString unsavedSessionAutosavePath()
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty())
        directory = QDir::home().filePath(QStringLiteral(".awesome-studio"));
    QDir().mkpath(directory);
    return QDir(directory).filePath(QStringLiteral("unsaved-session.autosave"));
}

QString projectBackupPath(const QString& projectPath, int generation)
{
    return projectPath + QStringLiteral(".bak%1").arg(generation);
}

QString projectPathForBackup(const QString& path)
{
    for (int generation = 1; generation <= 3; ++generation)
    {
        const QString backupSuffix = QStringLiteral(".bak%1").arg(generation);
        const QString suffix = QStringLiteral(".aws") + backupSuffix;
        if (path.endsWith(suffix, Qt::CaseInsensitive))
            return path.left(path.size() - backupSuffix.size());
    }
    return path;
}

bool stageProjectBackup(const QString& projectPath, QString& stagedPath, QString& error)
{
    stagedPath.clear();
    if (!QFileInfo::exists(projectPath))
        return true;

    QFile source(projectPath);
    if (!source.open(QIODevice::ReadOnly))
    {
        error = source.errorString();
        return false;
    }

    const QFileInfo projectInfo(projectPath);
    const QString temporaryTemplate = QDir(projectInfo.absolutePath()).filePath(
        QStringLiteral(".%1.backup-XXXXXX").arg(projectInfo.fileName()));
    QTemporaryFile staged(temporaryTemplate);
    if (!staged.open())
    {
        error = staged.errorString();
        return false;
    }

    QByteArray buffer(64 * 1024, '\0');
    while (!source.atEnd())
    {
        const qint64 bytesRead = source.read(buffer.data(), buffer.size());
        if (bytesRead < 0)
        {
            error = source.errorString();
            return false;
        }
        if (bytesRead == 0)
            break;
        if (staged.write(buffer.constData(), bytesRead) != bytesRead)
        {
            error = staged.errorString();
            return false;
        }
    }
    if (!staged.flush())
    {
        error = staged.errorString();
        return false;
    }

    stagedPath = staged.fileName();
    staged.setAutoRemove(false);
    staged.close();
    return true;
}

QString rotateProjectBackups(const QString& projectPath, const QString& stagedPath)
{
    QStringList errors;
    for (int generation = 3; generation >= 1; --generation)
    {
        const QString sourcePath = projectBackupPath(projectPath, generation);
        if (!QFileInfo::exists(sourcePath))
            continue;
        if (generation == 3)
        {
            if (!QFile::remove(sourcePath))
                errors.push_back(QObject::tr("could not remove %1").arg(QFileInfo(sourcePath).fileName()));
            continue;
        }

        const QString destinationPath = projectBackupPath(projectPath, generation + 1);
        if (QFileInfo::exists(destinationPath) && !QFile::remove(destinationPath))
        {
            errors.push_back(QObject::tr("could not replace %1").arg(QFileInfo(destinationPath).fileName()));
            continue;
        }
        if (!QFile::rename(sourcePath, destinationPath))
            errors.push_back(QObject::tr("could not rotate %1").arg(QFileInfo(sourcePath).fileName()));
    }

    const QString newestBackup = projectBackupPath(projectPath, 1);
    if (QFileInfo::exists(newestBackup) && !QFile::remove(newestBackup))
    {
        errors.push_back(QObject::tr("could not replace %1").arg(QFileInfo(newestBackup).fileName()));
    }
    else if (!QFile::rename(stagedPath, newestBackup))
    {
        errors.push_back(QObject::tr("could not install the newest backup"));
    }

    return errors.join(QStringLiteral("; "));
}

bool chooseWavExportOptions(QWidget* parent, WavExportOptions& options)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("WAV export format"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* sampleRateControl = new QComboBox(&dialog);
    sampleRateControl->addItem(QObject::tr("44.1 kHz"), 44100);
    sampleRateControl->addItem(QObject::tr("48 kHz"), 48000);
    sampleRateControl->addItem(QObject::tr("88.2 kHz"), 88200);
    sampleRateControl->addItem(QObject::tr("96 kHz"), 96000);
    sampleRateControl->addItem(QObject::tr("192 kHz"), 192000);
    const int sampleRateIndex = sampleRateControl->findData(options.sampleRate);
    if (sampleRateIndex >= 0)
        sampleRateControl->setCurrentIndex(sampleRateIndex);
    form->addRow(QObject::tr("Sample rate"), sampleRateControl);

    auto* formatControl = new QComboBox(&dialog);
    formatControl->addItem(QObject::tr("16-bit PCM"), static_cast<int>(WavSampleFormat::Pcm16));
    formatControl->addItem(QObject::tr("24-bit PCM"), static_cast<int>(WavSampleFormat::Pcm24));
    formatControl->addItem(QObject::tr("32-bit float"), static_cast<int>(WavSampleFormat::Float32));
    const int selectedIndex = formatControl->findData(static_cast<int>(options.sampleFormat));
    if (selectedIndex >= 0)
        formatControl->setCurrentIndex(selectedIndex);
    form->addRow(QObject::tr("Sample format"), formatControl);

    auto* ditherControl = new QCheckBox(QObject::tr("Add TPDF dither to integer PCM"), &dialog);
    ditherControl->setChecked(options.dither);
    form->addRow(ditherControl);
    layout->addLayout(form);
    QObject::connect(formatControl, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                     [formatControl, ditherControl](int)
    {
        ditherControl->setEnabled(formatControl->currentData().toInt() !=
                                  static_cast<int>(WavSampleFormat::Float32));
    });
    ditherControl->setEnabled(options.sampleFormat != WavSampleFormat::Float32);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    options.sampleRate = sampleRateControl->currentData().toInt();
    options.sampleFormat = static_cast<WavSampleFormat>(formatControl->currentData().toInt());
    options.dither = ditherControl->isEnabled() && ditherControl->isChecked();
    return true;
}

bool editPluginParameterAutomation(QWidget* parent, const QString& title,
                                   float minimum, float maximum,
                                   QVector<AutomationPoint>& points)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    auto* form = new QFormLayout;
    auto* beat = new QDoubleSpinBox(&dialog);
    beat->setRange(0.0, 16384.0);
    beat->setDecimals(3);
    beat->setSingleStep(1.0);
    beat->setSuffix(QObject::tr(" beats"));
    auto* value = new QDoubleSpinBox(&dialog);
    value->setRange(minimum, maximum);
    value->setDecimals(std::max(0, maximum - minimum > 100.0f ? 2 : 4));
    value->setSingleStep(std::max(0.001, static_cast<double>(maximum - minimum) / 100.0));
    form->addRow(QObject::tr("Position"), beat);
    form->addRow(QObject::tr("Value"), value);
    auto* keyframeButtons = new QHBoxLayout;
    auto* setPoint = new QPushButton(QObject::tr("Add / replace keyframe"), &dialog);
    auto* removePoint = new QPushButton(QObject::tr("Remove keyframe"), &dialog);
    keyframeButtons->addWidget(setPoint);
    keyframeButtons->addWidget(removePoint);
    layout->addWidget(list);
    layout->addLayout(form);
    layout->addLayout(keyframeButtons);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    const auto refreshList = [list, removePoint, &points]
    {
        list->clear();
        for (const AutomationPoint& point : points)
            list->addItem(QObject::tr("Beat %1  —  %2")
                .arg(point.beat, 0, 'f', 3).arg(point.value, 0, 'g', 6));
        removePoint->setEnabled(list->currentRow() >= 0);
    };
    refreshList();
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog,
                     [list, beat, value, removePoint, &points](int row)
    {
        const bool selected = row >= 0 && row < points.size();
        removePoint->setEnabled(selected);
        if (!selected)
            return;
        beat->setValue(points[row].beat);
        value->setValue(points[row].value);
    });
    QObject::connect(setPoint, &QPushButton::clicked, &dialog,
                     [list, beat, value, &points, refreshList]
    {
        const double targetBeat = beat->value();
        auto position = std::lower_bound(points.begin(), points.end(), targetBeat,
            [](const AutomationPoint& point, double target) { return point.beat < target; });
        if (position != points.end() && std::abs(position->beat - targetBeat) < 0.001)
            position->value = static_cast<float>(value->value());
        else if (position != points.begin() && std::abs((position - 1)->beat - targetBeat) < 0.001)
            (position - 1)->value = static_cast<float>(value->value());
        else if (points.size() < 10000)
            points.insert(position, {targetBeat, static_cast<float>(value->value())});
        refreshList();
        const auto selected = std::lower_bound(points.cbegin(), points.cend(), targetBeat,
            [](const AutomationPoint& point, double target) { return point.beat < target; });
        if (selected != points.cend())
            list->setCurrentRow(static_cast<int>(selected - points.cbegin()));
    });
    QObject::connect(removePoint, &QPushButton::clicked, &dialog,
                     [list, &points, refreshList]
    {
        const int row = list->currentRow();
        if (row < 0 || row >= points.size())
            return;
        points.removeAt(row);
        refreshList();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(440, 360);
    return dialog.exec() == QDialog::Accepted;
}

WavExportOptions& previousWavExportOptions()
{
    static WavExportOptions options;
    return options;
}

bool chooseMixerEffect(QWidget* parent, MixerEffectType type, MixerEffect& effect, QString& error)
{
    effect = defaultMixerEffect(type);
    if (!isPluginEffect(type))
        return true;

    if (type == MixerEffectType::Lv2Plugin)
    {
        const QVector<Lv2EffectDescriptor> plugins = Lv2AudioEffectInstance::availableEffects();
        if (plugins.isEmpty())
        {
            error = QObject::tr("No real-time-safe LV2 audio effects are installed.");
            return false;
        }
        QStringList choices;
        choices.reserve(plugins.size());
        for (const Lv2EffectDescriptor& plugin : plugins)
            choices.push_back(QStringLiteral("%1 — %2").arg(plugin.name, plugin.uri));
        bool accepted = false;
        const QString selected = QInputDialog::getItem(parent, QObject::tr("Add LV2 effect"),
            QObject::tr("Audio effect"), choices, 0, false, &accepted);
        if (!accepted)
            return false;
        const int index = choices.indexOf(selected);
        if (index < 0)
            return false;
        const Lv2EffectDescriptor& plugin = plugins[index];
        effect.pluginUri = plugin.uri;
        effect.pluginName = plugin.name;
        for (const Lv2EffectParameter& parameter : plugin.parameters)
            effect.pluginParameterValues.push_back(parameter.defaultValue);
        return true;
    }

    const QVector<ClapPluginDescriptor> plugins = ClapAudioPluginInstance::availablePlugins(
        ClapPluginKind::AudioEffect);
    if (plugins.isEmpty())
    {
        error = QObject::tr("No CLAP audio effects were found in the standard plug-in folders or CLAP_PATH.");
        return false;
    }
    QStringList choices;
    choices.reserve(plugins.size());
    for (const ClapPluginDescriptor& plugin : plugins)
        choices.push_back(QStringLiteral("%1 — %2 — %3")
                               .arg(plugin.name, plugin.vendor, plugin.id));
    bool accepted = false;
    const QString selected = QInputDialog::getItem(parent, QObject::tr("Add CLAP effect"),
        QObject::tr("Audio effect"), choices, 0, false, &accepted);
    if (!accepted)
        return false;
    const int index = choices.indexOf(selected);
    if (index < 0)
        return false;

    const ClapPluginDescriptor& plugin = plugins[index];
    effect.pluginUri = plugin.id;
    effect.pluginName = plugin.name;
    for (const ClapPluginParameter& parameter : plugin.parameters)
        effect.pluginParameterValues.push_back(parameter.defaultValue);
    return true;
}

bool chooseExportRange(QWidget* parent, double songLengthBeats, bool& useRange,
                       double& startBeat, double& endBeat)
{
    const double maximumBeat = std::max(0.25, songLengthBeats);
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("WAV export range"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* rangeEnabled = new QCheckBox(QObject::tr("Export a selected beat range"), &dialog);
    layout->addWidget(rangeEnabled);

    auto* rangeForm = new QFormLayout;
    auto* startControl = new QDoubleSpinBox(&dialog);
    auto* endControl = new QDoubleSpinBox(&dialog);
    for (QDoubleSpinBox* control : {startControl, endControl})
    {
        control->setDecimals(3);
        control->setSingleStep(1.0);
        control->setRange(0.0, maximumBeat);
        control->setSuffix(QObject::tr(" beats"));
        control->setEnabled(false);
    }
    startControl->setValue(0.0);
    endControl->setValue(maximumBeat);
    rangeForm->addRow(QObject::tr("Start (inclusive)"), startControl);
    rangeForm->addRow(QObject::tr("End (exclusive)"), endControl);
    layout->addLayout(rangeForm);
    layout->addWidget(new QLabel(QObject::tr(
        "Range exports contain exactly the selected interval; boundary effect tails are cut."), &dialog));

    QObject::connect(rangeEnabled, &QCheckBox::toggled, &dialog, [startControl, endControl](bool enabled)
    {
        startControl->setEnabled(enabled);
        endControl->setEnabled(enabled);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&dialog, rangeEnabled,
        startControl, endControl]
    {
        if (rangeEnabled->isChecked() && endControl->value() <= startControl->value())
        {
            QMessageBox::warning(&dialog, QObject::tr("Invalid export range"),
                                 QObject::tr("The end beat must be after the start beat."));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    useRange = rangeEnabled->isChecked();
    startBeat = startControl->value();
    endBeat = endControl->value();
    return true;
}

int drumSamplePadForPitch(int pitch)
{
    if (pitch == 35 || pitch == 36) return 0;
    if (pitch == 38 || pitch == 40) return 1;
    if (pitch == 42 || pitch == 44) return 2;
    if (pitch == 46) return 3;
    return -1;
}

int midiExportChannelForTrack(const ProjectData& project, quint64 targetTrackId)
{
    int instrumentChannel = 0;
    for (const InstrumentTrack& track : project.tracks)
    {
        if (track.instrumentType == InstrumentType::Audio)
            continue;
        int channel = 9;
        if (track.instrumentType == InstrumentType::Synth ||
            track.instrumentType == InstrumentType::ClapInstrument)
        {
            const int channelSlot = instrumentChannel++ % 15;
            channel = channelSlot < 9 ? channelSlot : channelSlot + 1;
        }
        if (track.id == targetTrackId)
            return channel;
    }
    return 0;
}

bool sameAutomationPoints(const QVector<AutomationPoint>& left, const QVector<AutomationPoint>& right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype index = 0; index < left.size(); ++index)
    {
        if (std::abs(left[index].beat - right[index].beat) > 1.0e-9 ||
            std::abs(left[index].value - right[index].value) > 1.0e-6f ||
            left[index].curveToNext != right[index].curveToNext)
            return false;
    }
    return true;
}

bool upsertAutomationPoint(QVector<AutomationPoint>& points, double beat, float value)
{
    auto position = std::lower_bound(points.begin(), points.end(), beat,
        [](const AutomationPoint& point, double targetBeat) { return point.beat < targetBeat; });
    if (position != points.end() && std::abs(position->beat - beat) < 0.001)
    {
        position->value = value;
        return true;
    }
    if (position != points.begin() && std::abs((position - 1)->beat - beat) < 0.001)
    {
        (position - 1)->value = value;
        return true;
    }
    if (points.size() >= 10000)
        return false;
    points.insert(position, {beat, value});
    return true;
}

double audioRegionEndBeat(const ProjectData& project, const MidiClip& clip)
{
    double durationBeats = clip.lengthBeats;
    if (clip.audioSource == nullptr || clip.audioSource->sampleRate <= 0 ||
        clip.audioStartFrame < 0 || clip.audioStartFrame >= clip.audioSource->frameCount())
        return clip.startBeat;
    const double sourceEndSeconds = project.secondsAtBeat(clip.startBeat) +
        static_cast<double>(clip.audioSource->frameCount() - clip.audioStartFrame) /
        clip.audioSource->sampleRate;
    const double sourceEndBeat = project.beatAtSeconds(sourceEndSeconds);
    const double sourceDurationBeats = std::max(0.0, sourceEndBeat - clip.startBeat);
    durationBeats = std::min(durationBeats, sourceDurationBeats);
    return clip.startBeat + std::max(0.0, durationBeats);
}

QVector<const MidiClip*> overlappingAudioRegions(const ProjectData& project,
                                                  const InstrumentTrack& track,
                                                  const MidiClip& selected)
{
    QVector<const MidiClip*> overlaps;
    if (!selected.isAudioRegion() || selected.audioSource == nullptr)
        return overlaps;
    for (const MidiClip& candidate : track.clips)
    {
        if (candidate.id == selected.id || !candidate.isAudioRegion() || candidate.audioSource == nullptr ||
            std::abs(candidate.startBeat - selected.startBeat) < 1.0e-6)
            continue;
        const bool selectedIsOutgoing = selected.startBeat < candidate.startBeat;
        const MidiClip& outgoing = selectedIsOutgoing ? selected : candidate;
        const MidiClip& incoming = selectedIsOutgoing ? candidate : selected;
        const double outgoingEnd = audioRegionEndBeat(project, outgoing);
        const double incomingEnd = audioRegionEndBeat(project, incoming);
        if (incomingEnd <= outgoingEnd + 1.0e-6)
            continue;
        const double overlap = outgoingEnd - incoming.startBeat;
        if (overlap > 1.0e-6)
            overlaps.push_back(&candidate);
    }
    return overlaps;
}
}

class NoteEditCommand final : public QUndoCommand
{
public:
    NoteEditCommand(MainWindow* window, quint64 trackId, quint64 clipId,
                    QVector<MidiNote> before, QVector<MidiNote> after,
                    double beforeLengthBeats = -1.0, double afterLengthBeats = -1.0,
                    QVector<MidiPitchBend> beforePitchBends = {},
                    QVector<MidiPitchBend> afterPitchBends = {}, bool includePitchBends = false,
                    QString commandText = {})
        : window(window), trackId(trackId), clipId(clipId),
          before(std::move(before)), after(std::move(after)),
          beforeLengthBeats(beforeLengthBeats), afterLengthBeats(afterLengthBeats),
          beforePitchBends(std::move(beforePitchBends)),
          afterPitchBends(std::move(afterPitchBends)), includePitchBends(includePitchBends)
    {
        if (commandText.isEmpty())
            commandText = includePitchBends ? QObject::tr("Record MIDI take")
                                            : QObject::tr("Edit MIDI notes");
        setText(commandText);
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyNoteHistory(trackId, clipId, before, beforeLengthBeats,
                                     includePitchBends ? &beforePitchBends : nullptr);
    }

    void redo() override
    {
        if (firstRedo)
        {
            firstRedo = false;
            return;
        }
        if (window != nullptr)
            window->applyNoteHistory(trackId, clipId, after, afterLengthBeats,
                                     includePitchBends ? &afterPitchBends : nullptr);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    quint64 clipId;
    QVector<MidiNote> before;
    QVector<MidiNote> after;
    double beforeLengthBeats;
    double afterLengthBeats;
    QVector<MidiPitchBend> beforePitchBends;
    QVector<MidiPitchBend> afterPitchBends;
    bool includePitchBends = false;
    bool firstRedo = true;
};

class StepDivisionCommand final : public QUndoCommand
{
public:
    StepDivisionCommand(MainWindow* window, quint64 trackId, quint64 clipId,
                        int before, int after)
        : window(window), trackId(trackId), clipId(clipId), before(before), after(after)
    {
        setText(QObject::tr("Change drum step division"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyStepDivisionHistory(trackId, clipId, before);
    }

    void redo() override
    {
        if (firstRedo)
        {
            firstRedo = false;
            return;
        }
        if (window != nullptr)
            window->applyStepDivisionHistory(trackId, clipId, after);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    quint64 clipId;
    int before;
    int after;
    bool firstRedo = true;
};

class PitchBendRangeCommand final : public QUndoCommand
{
public:
    PitchBendRangeCommand(MainWindow* window, double before, double after)
        : window(window), before(before), after(after)
    {
        setText(QObject::tr("Change MIDI pitch-bend range"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyPitchBendRangeHistory(before);
    }

    void redo() override
    {
        if (window != nullptr)
            window->applyPitchBendRangeHistory(after);
    }

private:
    QPointer<MainWindow> window;
    double before;
    double after;
};

class AutomationEditCommand final : public QUndoCommand
{
public:
    AutomationEditCommand(MainWindow* window, quint64 trackId, int parameter,
                          QVector<AutomationPoint> before, QVector<AutomationPoint> after,
                          QString pluginUri = {})
        : window(window), trackId(trackId), parameter(parameter),
          before(std::move(before)), after(std::move(after)), pluginUri(std::move(pluginUri))
    {
        setText(QObject::tr("Edit track automation"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyAutomationHistory(trackId, parameter, pluginUri, before);
    }

    void redo() override
    {
        if (window != nullptr)
            window->applyAutomationHistory(trackId, parameter, pluginUri, after);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    int parameter;
    QVector<AutomationPoint> before;
    QVector<AutomationPoint> after;
    QString pluginUri;
};

class MarkerEditCommand final : public QUndoCommand
{
public:
    MarkerEditCommand(MainWindow* window, QVector<TimelineMarker> before, QVector<TimelineMarker> after)
        : window(window), before(std::move(before)), after(std::move(after))
    {
        setText(QObject::tr("Edit arrangement markers"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyMarkerHistory(before);
    }

    void redo() override
    {
        if (window != nullptr)
            window->applyMarkerHistory(after);
    }

private:
    QPointer<MainWindow> window;
    QVector<TimelineMarker> before;
    QVector<TimelineMarker> after;
};

class TempoMapEditCommand final : public QUndoCommand
{
public:
    TempoMapEditCommand(MainWindow* window, double beforeBaseTempo,
                        QVector<TempoChange> before, double afterBaseTempo,
                        QVector<TempoChange> after)
        : window(window), beforeBaseTempo(beforeBaseTempo), before(std::move(before)),
          afterBaseTempo(afterBaseTempo), after(std::move(after))
    {
        setText(QObject::tr("Edit tempo map"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyTempoHistory(beforeBaseTempo, before);
    }

    void redo() override
    {
        if (window != nullptr)
            window->applyTempoHistory(afterBaseTempo, after);
    }

private:
    QPointer<MainWindow> window;
    double beforeBaseTempo;
    QVector<TempoChange> before;
    double afterBaseTempo;
    QVector<TempoChange> after;
};

class AudioFadeEditCommand final : public QUndoCommand
{
public:
    AudioFadeEditCommand(MainWindow* window, quint64 trackId,
                         QVector<MainWindow::AudioFadeState> before,
                         QVector<MainWindow::AudioFadeState> after)
        : window(window), trackId(trackId), before(std::move(before)), after(std::move(after))
    {
        setText(QObject::tr("Create audio crossfade"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyAudioFadeHistory(trackId, before);
    }

    void redo() override
    {
        if (window != nullptr)
            window->applyAudioFadeHistory(trackId, after);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    QVector<MainWindow::AudioFadeState> before;
    QVector<MainWindow::AudioFadeState> after;
};

MainWindow::MainWindow(AudioEngine& engine, QWidget* parent)
    : QMainWindow(parent), audioEngine(engine)
{
    setObjectName(QStringLiteral("mainWindow"));
    resize(1480, 920);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    editorTabs = new QTabWidget(this);
    arrangementEditor = new ArrangementEditor(editorTabs);
    midiEditor = new MidiEditor(editorTabs);
    stepSequencer = new StepSequencer(editorTabs);
    editorTabs->addTab(arrangementEditor, tr("Playlist"));
    editorTabs->addTab(midiEditor, tr("Piano roll"));
    editorTabs->addTab(stepSequencer, tr("Step sequencer"));
    setCentralWidget(editorTabs);

    createActions();
    createToolBars();
    createDocks();
    createMenus();
    loadMidiControllerMappings();

    const auto applyEditedNotes = [this](const QVector<MidiNote>& before, const QVector<MidiNote>& after)
    {
        MidiClip* clip = selectedClip();
        if (clip == nullptr || selectedTrackId == 0)
            return;
        clip->notes = after;
        audioEngine.setProject(project);
        arrangementEditor->setProject(project);
        arrangementEditor->setSelection(selectedTrackId, selectedClipId);
        midiEditor->setNotes(after);
        stepSequencer->setPattern(after, clip->lengthBeats, project.quarterNotesPerBar(),
                                  project.nextNoteId(), true, clip->stepsPerBar);
        undoStack.push(new NoteEditCommand(this, selectedTrackId, selectedClipId, before, after));
        refreshDirtyState();
        statusBar()->showMessage(tr("MIDI clip updated"), 1600);
    };
    connect(midiEditor, &MidiEditor::notesEdited, this, applyEditedNotes);
    connect(stepSequencer, &StepSequencer::notesEdited, this, applyEditedNotes);
    connect(stepSequencer, &StepSequencer::stepsPerBarChanged, this,
            [this](int before, int after)
    {
        MidiClip* clip = selectedClip();
        const InstrumentTrack* track = project.findTrack(selectedTrackId);
        if (clip == nullptr || track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
            clip->isAudioRegion() || before == after)
            return;

        const quint64 trackId = selectedTrackId;
        const quint64 clipId = selectedClipId;
        clip->stepsPerBar = after;
        audioEngine.setProject(project);
        undoStack.push(new StepDivisionCommand(this, trackId, clipId, before, after));
        refreshDirtyState();
        statusBar()->showMessage(tr("Drum step division updated"), 1600);
    });
    connect(midiEditor, &MidiEditor::pitchBendsEdited, this,
            [this](const QVector<MidiPitchBend>& before, const QVector<MidiPitchBend>& after)
    {
        MidiClip* clip = selectedClip();
        if (clip == nullptr || clip->isAudioRegion() || selectedTrackId == 0)
            return;
        const quint64 trackId = selectedTrackId;
        const quint64 clipId = selectedClipId;
        const QVector<MidiNote> notes = clip->notes;
        clip->pitchBends = after;
        audioEngine.setProject(project);
        arrangementEditor->setProject(project);
        arrangementEditor->setSelection(trackId, clipId);
        undoStack.push(new NoteEditCommand(this, trackId, clipId, notes, notes,
            -1.0, -1.0, before, after, true, tr("Edit MIDI pitch bend")));
        refreshDirtyState();
        statusBar()->showMessage(tr("MIDI pitch bend updated"), 1600);
    });
    connect(midiEditor, &MidiEditor::previewRequested, this, [this](int pitch)
    {
        const InstrumentTrack* track = selectedTrack();
        if (track != nullptr && track->instrumentType == InstrumentType::ClapInstrument)
        {
            audioEngine.triggerClapInstrumentPreview(pitch, 96, *track);
            return;
        }
        const int pad = track != nullptr && track->instrumentType == InstrumentType::DrumRack
            ? drumSamplePadForPitch(pitch) : -1;
        const DrumPadSample* padSample = pad >= 0
            ? &track->drumSamples[static_cast<size_t>(pad)] : nullptr;
        if (padSample != nullptr)
        {
            if (padSample->hasSampleAssignment())
            {
                const auto& previewSample = padSample->sourceForVelocity(96);
                if (previewSample != nullptr)
                {
                    const DrumLayerSettings settings = padSample->settingsForVelocity(96);
                    audioEngine.triggerSamplePreview(previewSample, 96, track->pan, track->gain,
                                                     settings.startPositionPercent,
                                                     settings.tuningSemitones, settings.loopEnabled,
                                                     settings.attackSeconds, settings.decaySeconds,
                                                     settings.sustainLevel, settings.releaseSeconds);
                }
                else
                    statusBar()->showMessage(tr("The assigned drum sample is offline"), 2000);
                return;
            }
        }
        audioEngine.triggerPreview(pitch, 96,
                                   track == nullptr ? SynthWaveform::Sine : track->waveform,
                                   track == nullptr ? 0.0f : track->pan,
                                   track != nullptr && track->instrumentType == InstrumentType::DrumRack,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth &&
                                       track->secondOscillatorEnabled,
                                   track == nullptr ? SynthWaveform::Saw : track->secondOscillatorWaveform,
                                   track == nullptr ? 0.0f : track->secondOscillatorDetuneCents,
                                   track == nullptr ? 0.5f : track->secondOscillatorMix,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth &&
                                       track->synthFilterEnabled,
                                   track == nullptr ? 20000.0f : track->synthFilterCutoffHz,
                                   track == nullptr ? 0.707f : track->synthFilterResonance,
                                   track == nullptr ? SynthFilterType::LowPass : track->synthFilterType,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth &&
                                       track->synthFilterEnabled &&
                                       track->synthFilterEnvelopeEnabled,
                                   track == nullptr ? 24.0f :
                                       track->synthFilterEnvelopeAmountSemitones,
                                   track == nullptr ? 0.008f :
                                       track->synthFilterEnvelopeAttackSeconds,
                                   track == nullptr ? 0.2f :
                                       track->synthFilterEnvelopeDecaySeconds,
                                   track == nullptr ? 0.0f :
                                       track->synthFilterEnvelopeSustainLevel,
                                   track == nullptr ? 0.2f :
                                       track->synthFilterEnvelopeReleaseSeconds,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->attackSeconds
                                       : padSample == nullptr ? 0.008f : padSample->attackSeconds,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->decaySeconds
                                       : padSample == nullptr ? 0.0f : padSample->decaySeconds,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->sustainLevel
                                       : padSample == nullptr ? 1.0f : padSample->sustainLevel,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->releaseSeconds
                                       : padSample == nullptr ? 0.06f : padSample->releaseSeconds);
    });
    connect(stepSequencer, &StepSequencer::previewRequested, this, [this](int pitch)
    {
        const InstrumentTrack* track = selectedTrack();
        if (track != nullptr && track->instrumentType == InstrumentType::ClapInstrument)
        {
            audioEngine.triggerClapInstrumentPreview(pitch, 100, *track);
            return;
        }
        const int pad = track != nullptr && track->instrumentType == InstrumentType::DrumRack
            ? drumSamplePadForPitch(pitch) : -1;
        const DrumPadSample* padSample = pad >= 0
            ? &track->drumSamples[static_cast<size_t>(pad)] : nullptr;
        if (padSample != nullptr)
        {
            if (padSample->hasSampleAssignment())
            {
                const auto& previewSample = padSample->sourceForVelocity(100);
                if (previewSample != nullptr)
                {
                    const DrumLayerSettings settings = padSample->settingsForVelocity(100);
                    audioEngine.triggerSamplePreview(previewSample, 100, track->pan, track->gain,
                                                     settings.startPositionPercent,
                                                     settings.tuningSemitones, settings.loopEnabled,
                                                     settings.attackSeconds, settings.decaySeconds,
                                                     settings.sustainLevel, settings.releaseSeconds);
                }
                else
                    statusBar()->showMessage(tr("The assigned drum sample is offline"), 2000);
                return;
            }
        }
        audioEngine.triggerPreview(pitch, 100,
                                   track == nullptr ? SynthWaveform::Sine : track->waveform,
                                   track == nullptr ? 0.0f : track->pan,
                                   track != nullptr && track->instrumentType == InstrumentType::DrumRack,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth &&
                                       track->secondOscillatorEnabled,
                                   track == nullptr ? SynthWaveform::Saw : track->secondOscillatorWaveform,
                                   track == nullptr ? 0.0f : track->secondOscillatorDetuneCents,
                                   track == nullptr ? 0.5f : track->secondOscillatorMix,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth &&
                                       track->synthFilterEnabled,
                                   track == nullptr ? 20000.0f : track->synthFilterCutoffHz,
                                   track == nullptr ? 0.707f : track->synthFilterResonance,
                                   track == nullptr ? SynthFilterType::LowPass : track->synthFilterType,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth &&
                                       track->synthFilterEnabled &&
                                       track->synthFilterEnvelopeEnabled,
                                   track == nullptr ? 24.0f :
                                       track->synthFilterEnvelopeAmountSemitones,
                                   track == nullptr ? 0.008f :
                                       track->synthFilterEnvelopeAttackSeconds,
                                   track == nullptr ? 0.2f :
                                       track->synthFilterEnvelopeDecaySeconds,
                                   track == nullptr ? 0.0f :
                                       track->synthFilterEnvelopeSustainLevel,
                                   track == nullptr ? 0.2f :
                                       track->synthFilterEnvelopeReleaseSeconds,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->attackSeconds
                                       : padSample == nullptr ? 0.008f : padSample->attackSeconds,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->decaySeconds
                                       : padSample == nullptr ? 0.0f : padSample->decaySeconds,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->sustainLevel
                                       : padSample == nullptr ? 1.0f : padSample->sustainLevel,
                                   track != nullptr && track->instrumentType == InstrumentType::Synth
                                       ? track->releaseSeconds
                                       : padSample == nullptr ? 0.06f : padSample->releaseSeconds);
    });
    connect(arrangementEditor, &ArrangementEditor::clipActivated, this,
            [this](quint64 trackId, quint64 clipId) { selectClip(trackId, clipId, true); });
    connect(arrangementEditor, &ArrangementEditor::clipSelected, this,
            [this](quint64 trackId, quint64 clipId) { selectClip(trackId, clipId, false); });
    connect(arrangementEditor, &ArrangementEditor::clipAdded, this,
            [this](quint64 trackId, double startBeat) { addClipAt(trackId, startBeat); });
    connect(arrangementEditor, &ArrangementEditor::clipRemoved, this,
            [this](quint64 trackId, quint64 clipId) { removeClip(trackId, clipId); });
    connect(arrangementEditor, &ArrangementEditor::clipMoved, this,
            [this](quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat)
            { moveClip(sourceTrackId, targetTrackId, clipId, startBeat); });
    connect(arrangementEditor, &ArrangementEditor::audioRegionTrimmed, this,
            [this](quint64 trackId, quint64 clipId, double startBeat, double lengthBeats, qint64 audioStartFrame)
            { trimAudioRegion(trackId, clipId, startBeat, lengthBeats, audioStartFrame); });
    connect(arrangementEditor, &ArrangementEditor::audioRegionRelinkRequested, this,
            &MainWindow::relinkAudioRegion);
    connect(arrangementEditor, &ArrangementEditor::automationChanged, this,
            [this](quint64 trackId, int parameter, const QString& pluginUri,
                   const QVector<AutomationPoint>& before,
                   const QVector<AutomationPoint>& after)
    {
        if (project.findTrack(trackId) == nullptr)
            return;
        undoStack.push(new AutomationEditCommand(this, trackId, parameter, before, after, pluginUri));
    });
    connect(arrangementEditor, &ArrangementEditor::automationPointLimitReached, this, [this]
    {
        statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
    });
    connect(arrangementEditor, &ArrangementEditor::markerAddRequested,
            this, &MainWindow::addTimelineMarker);
    connect(arrangementEditor, &ArrangementEditor::markerRenameRequested,
            this, &MainWindow::renameTimelineMarker);
    connect(arrangementEditor, &ArrangementEditor::markerRemoveRequested,
            this, &MainWindow::removeTimelineMarker);
    connect(arrangementEditor, &ArrangementEditor::markerSeekRequested,
            this, &MainWindow::seekToTimelineMarker);
    connect(arrangementEditor, &ArrangementEditor::tempoChangeRequested,
            this, &MainWindow::setTimelineTempo);
    connect(arrangementEditor, &ArrangementEditor::tempoRemoveRequested,
            this, &MainWindow::removeTimelineTempo);
    connect(arrangementEditor, &ArrangementEditor::trackSelected, this,
            [this](quint64 trackId) { selectTrack(trackId); });
    connect(&undoStack, &QUndoStack::cleanChanged, this, [this] { refreshDirtyState(); });

    setStyleSheet(QStringLiteral(
        "QMainWindow { background: #12161c; color: #e8edf2; }"
        "QToolBar { background: #1b2028; border: 0; border-bottom: 1px solid #303743; spacing: 6px; padding: 5px; }"
        "QToolBar QToolButton { color: #d8e0e8; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 5px 8px; }"
        "QToolBar QToolButton:hover { background: #29313b; border-color: #3b4653; }"
        "QToolBar QToolButton:checked { color: #101d20; background: #56d3c0; }"
        "QMenuBar, QMenu { background: #1b2028; color: #dce3e9; }"
        "QMenuBar::item:selected, QMenu::item:selected { background: #303944; }"
        "QDockWidget { color: #dce3e9; font-weight: 600; }"
        "QDockWidget::title { background: #1b2028; padding: 7px 9px; border-bottom: 1px solid #303743; }"
        "QStatusBar { background: #1b2028; color: #9da8b4; border-top: 1px solid #303743; }"
        "QComboBox, QDoubleSpinBox { color: #e8edf2; background: #252c35; border: 1px solid #3b4653; border-radius: 4px; padding: 4px 7px; min-height: 20px; }"
        "QComboBox QAbstractItemView { background: #252c35; color: #e8edf2; selection-background-color: #27796f; }"
        "QListWidget { background: #171c23; color: #cbd4dd; border: 0; outline: 0; }"
        "QListWidget::item { padding: 6px 8px; border-radius: 3px; }"
        "QListWidget::item:selected { background: #2a4d4d; color: #dbfff8; }"
        "QGroupBox { color: #dce4eb; border: 1px solid #343d48; border-radius: 4px; margin-top: 8px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
        "QSlider::groove:vertical { width: 6px; background: #303944; border-radius: 3px; }"
        "QSlider::handle:vertical { height: 16px; margin: 0 -5px; background: #56d3c0; border-radius: 4px; }"
        "QSlider::groove:horizontal { height: 5px; background: #303944; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -4px 0; background: #56d3c0; border-radius: 3px; }"
    ));

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());

    if (!project.tracks.isEmpty())
    {
        selectedTrackId = project.tracks.first().id;
        if (!project.tracks.first().clips.isEmpty())
            selectedClipId = project.tracks.first().clips.first().id;
    }
    refreshProjectUi();
    offerUnsavedSessionRecovery();

    if (audioEngine.initialize())
    {
        audioDeviceLabel->setText(tr("Audio · %1").arg(audioEngine.deviceName()));
        statusBar()->showMessage(tr("Ready · add clips in Playlist, then double-click one to edit MIDI"));
    }
    else
    {
        audioDeviceLabel->setText(tr("Audio unavailable"));
        statusBar()->showMessage(audioEngine.errorMessage() +
                                 tr(" The editor remains available; connect an output device to audition."));
    }
    refreshAudioInputs();

    updateProjectTitle();
    uiTimer = new QTimer(this);
    uiTimer->setInterval(33);
    connect(uiTimer, &QTimer::timeout, this, [this]
    {
        audioEngine.servicePluginCallbacks();
        const double beat = audioEngine.positionBeats();
        if (automationWriteSession.active)
        {
            if (!audioEngine.isPlaying())
                finishAutomationWrite();
            else if (automationWriteModeControl->currentData().toInt() == 1)
                recordAutomationValue(automationWriteSession.trackId,
                                      automationWriteSession.parameter,
                                      automationWriteSession.lastValue);
        }
        if (recordingPending)
        {
            if (!audioEngine.isPlaying())
            {
                finishAudioRecording(false);
                statusBar()->showMessage(tr("Playback stopped before recording could start."), 4000);
            }
            else if (beat >= 0.0 && !startPendingAudioRecording(beat))
            {
                finishAudioRecording(false);
            }
        }
        if (audioRecorder)
        {
            const QString inputIssue = audioRecorder->errorMessage();
            if (audioRecorder->maximumReached())
                finishAudioRecording(true);
            else if (!inputIssue.isEmpty())
                finishAudioRecording(true);
            else if (recordingTransportActive && !audioEngine.isPlaying())
                finishAudioRecording(true);
        }
        if (audioMonitor)
        {
            const QString monitorIssue = audioMonitor->errorMessage();
            if (!monitorIssue.isEmpty())
                finishAudioMonitoring();
        }
        if (midiRecordingPending)
        {
            if (!midiTransportActive || !audioEngine.isPlaying())
            {
                finishMidiRecording(false);
                statusBar()->showMessage(tr("Playback stopped before MIDI recording could start."), 4000);
            }
            else if (beat >= 0.0 && !startPendingMidiRecording(beat))
            {
                finishMidiRecording(false);
            }
        }
        if (midiRecordingActive && midiTransportActive && !audioEngine.isPlaying())
            finishMidiRecording(true);
        if (beat < 0.0)
        {
            playheadLabel->setText(tr("COUNT-IN"));
            arrangementEditor->setPlayheadBeat(-1.0);
        }
        else
        {
            const double quarterNotesPerBar = project.quarterNotesPerBar();
            const double denominatorUnit = 4.0 / project.beatUnit;
            const int bar = static_cast<int>(beat / quarterNotesPerBar) + 1;
            const double beatInBar = std::fmod(beat, quarterNotesPerBar) / denominatorUnit + 1.0;
            playheadLabel->setText(tr("BAR %1  ·  BEAT %2")
                                       .arg(bar, 2, 10, QLatin1Char('0'))
                                       .arg(beatInBar, 0, 'f', 2));
            arrangementEditor->setPlayheadBeat(beat);
        }
        const MidiClip* clip = selectedClip();
        if (beat >= 0.0 && clip != nullptr && clip->lengthBeats > 0.0)
            midiEditor->setPlayheadBeat(std::fmod(std::max(0.0, beat - clip->startBeat), clip->lengthBeats));
        else
            midiEditor->setPlayheadBeat(0.0);
        if (!audioEngine.isPlaying() && playAction->isChecked())
            playAction->setChecked(false);
        refreshMixerMeters();
    });
    uiTimer->start();
    autosaveTimer = new QTimer(this);
    autosaveTimer->setInterval(120000);
    connect(autosaveTimer, &QTimer::timeout, this, &MainWindow::autosaveProject);
    autosaveTimer->start();
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
    auto* newAction = new QAction(style()->standardIcon(QStyle::SP_FileIcon), tr("New"), this);
    newAction->setShortcut(QKeySequence::New);
    newAction->setStatusTip(tr("Create a new arrangement"));
    connect(newAction, &QAction::triggered, this, [this]
    {
        if (maybeSave())
            setProject(ProjectData{});
    });

    auto* openAction = new QAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Open"), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this]
    {
        if (!maybeSave())
            return;
        const QString selectedPath = QFileDialog::getOpenFileName(
            this, tr("Open project"), projectPath,
            tr("Awesome Studio project (*.aws *.aws.bak1 *.aws.bak2 *.aws.bak3)"));
        if (selectedPath.isEmpty())
            return;
        const QString path = projectPathForBackup(selectedPath);
        const bool recoveredBackup = path != selectedPath;
        QString loadPath = selectedPath;
        const QFileInfo autosaveInfo(projectAutosavePath(path));
        const QFileInfo projectInfo(path);
        bool recoveredAutosave = false;
        if (!recoveredBackup && autosaveInfo.exists() &&
            autosaveInfo.lastModified() >= projectInfo.lastModified())
        {
            const QMessageBox::StandardButton choice = QMessageBox::question(
                this, tr("Recover autosave"),
                tr("A newer autosave is available for %1. Recover it?")
                    .arg(QFileInfo(path).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (choice == QMessageBox::Yes)
            {
                loadPath = autosaveInfo.filePath();
                recoveredAutosave = true;
            }
            else
            {
                QFile::remove(autosaveInfo.filePath());
            }
        }
        ProjectData loaded;
        QString error;
        if (!ProjectFile::load(loadPath, loaded, &error) && recoveredAutosave)
        {
            QMessageBox::warning(this, tr("Autosave could not be recovered"),
                                 tr("The original project will be opened instead.\n%1").arg(error));
            QFile::remove(projectAutosavePath(path));
            recoveredAutosave = false;
            error.clear();
            if (!ProjectFile::load(path, loaded, &error))
            {
                QMessageBox::critical(this, tr("Could not open project"), error);
                return;
            }
        }
        else if (!error.isEmpty())
        {
            QMessageBox::critical(this, tr("Could not open project"), error);
            return;
        }
        setProject(loaded, path);
        if (recoveredAutosave)
        {
            settingsDirty = true;
            refreshDirtyState();
            statusBar()->showMessage(tr("Recovered autosave for %1; save the project to keep it.")
                                         .arg(QFileInfo(path).fileName()), 6000);
        }
        else if (recoveredBackup)
        {
            settingsDirty = true;
            refreshDirtyState();
            statusBar()->showMessage(tr("Opened backup for %1; save to restore it as the current project.")
                                         .arg(QFileInfo(path).fileName()), 6000);
        }
    });

    auto* saveAction = new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Save"), this);
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, [this]
    {
        if (projectPath.isEmpty())
        {
            QString path = QFileDialog::getSaveFileName(this, tr("Save project"), QString(),
                                                         tr("Awesome Studio project (*.aws)"));
            if (!path.isEmpty())
            {
                if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".aws");
                saveToPath(path);
            }
        }
        else
        {
            saveToPath(projectPath);
        }
    });

    auto* exportAction = new QAction(tr("Export arrangement to WAV…"), this);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportAudio);

    addTrackAction = new QAction(tr("Add track"), this);
    connect(addTrackAction, &QAction::triggered, this, &MainWindow::addTrack);
    addDrumTrackAction = new QAction(tr("Add drum track"), this);
    connect(addDrumTrackAction, &QAction::triggered, this, &MainWindow::addDrumTrack);
#if defined(AWESOME_STUDIO_HAS_CLAP)
    addClapInstrumentTrackAction = new QAction(tr("Add CLAP instrument"), this);
    connect(addClapInstrumentTrackAction, &QAction::triggered,
            this, &MainWindow::addClapInstrumentTrack);
#endif
    addPatternAction = new QAction(tr("New pattern"), this);
    connect(addPatternAction, &QAction::triggered, this, &MainWindow::addNewPattern);

    playAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), this);
    playAction->setCheckable(true);
    playAction->setShortcut(Qt::Key_Space);
    playAction->setToolTip(tr("Play arrangement (Space)"));
    connect(playAction, &QAction::triggered, this, [this](bool enabled)
    {
        if (enabled)
        {
            if (!audioEngine.isAudioAvailable() && !audioEngine.initialize())
            {
                playAction->setChecked(false);
                audioDeviceLabel->setText(tr("Audio unavailable"));
                statusBar()->showMessage(audioEngine.errorMessage(), 5000);
                return;
            }
            audioEngine.setProject(project);
            audioEngine.startPlayback();
            statusBar()->showMessage(tr("Playing arrangement"));
        }
        else
        {
            finishAutomationWrite();
            if (audioRecorder || recordingPending)
                finishAudioRecording(true);
            if (midiRecordingActive || midiRecordingPending)
                finishMidiRecording(true);
            audioEngine.stopPlayback();
            midiEditor->setPlayheadBeat(0.0);
            arrangementEditor->setPlayheadBeat(0.0);
            statusBar()->showMessage(tr("Playback stopped"), 1800);
        }
    });

    stopAction = new QAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"), this);
    stopAction->setShortcut(Qt::Key_Escape);
    connect(stopAction, &QAction::triggered, this, [this]
    {
        finishAutomationWrite();
        if (audioRecorder || recordingPending)
            finishAudioRecording(true);
        if (midiRecordingActive || midiRecordingPending)
            finishMidiRecording(true);
        audioEngine.stopPlayback();
        playAction->setChecked(false);
        midiEditor->setPlayheadBeat(0.0);
        arrangementEditor->setPlayheadBeat(0.0);
    });

    recordAction = new QAction(tr("Record audio…"), this);
    recordAction->setCheckable(true);
    recordAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    recordAction->setToolTip(tr("Record the selected audio input to a new Playlist track"));
    connect(recordAction, &QAction::triggered, this, [this](bool enabled)
    {
        if (enabled)
            startAudioRecording();
        else
            finishAudioRecording(true);
    });

    audioMonitorAction = new QAction(tr("Monitor audio"), this);
    audioMonitorAction->setCheckable(true);
    audioMonitorAction->setEnabled(false);
    audioMonitorAction->setToolTip(tr("Hear the selected audio input through the master output"));
    connect(audioMonitorAction, &QAction::toggled, this, &MainWindow::setAudioMonitoring);

    midiRecordAction = new QAction(tr("Record MIDI"), this);
    midiRecordAction->setCheckable(true);
    midiRecordAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    midiRecordAction->setToolTip(tr("Record MIDI notes from the selected input into an instrument pattern"));
    connect(midiRecordAction, &QAction::triggered, this, [this](bool enabled)
    {
        if (enabled)
            startMidiRecording();
        else
            finishMidiRecording(true);
    });

    midiMonitorAction = new QAction(tr("Monitor MIDI"), this);
    midiMonitorAction->setCheckable(true);
    midiMonitorAction->setToolTip(tr("Play incoming MIDI on the selected synth, drum rack, or CLAP instrument without recording"));
    connect(midiMonitorAction, &QAction::toggled, this, &MainWindow::setMidiMonitorEnabled);

    loopAction = new QAction(tr("Loop"), this);
    loopAction->setCheckable(true);
    loopAction->setChecked(project.loopEnabled);
    loopAction->setToolTip(tr("Loop the full arrangement"));
    connect(loopAction, &QAction::toggled, this, [this](bool enabled)
    {
        project.loopEnabled = enabled;
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    metronomeAction = new QAction(tr("Metronome"), this);
    metronomeAction->setCheckable(true);
    metronomeAction->setChecked(project.metronomeEnabled);
    metronomeAction->setToolTip(tr("Play an accented click on each beat"));
    connect(metronomeAction, &QAction::toggled, this, [this](bool enabled)
    {
        project.metronomeEnabled = enabled;
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    undoAction = undoStack.createUndoAction(this, tr("Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    redoAction = undoStack.createRedoAction(this, tr("Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
}

void MainWindow::createToolBars()
{
    auto* projectBar = addToolBar(tr("Project"));
    projectBar->setObjectName(QStringLiteral("projectToolbar"));
    for (QAction* action : findChildren<QAction*>())
    {
        if (action->text() == tr("New") || action->text() == tr("Open") || action->text() == tr("Save"))
            projectBar->addAction(action);
    }
    projectBar->setMovable(false);

    auto* arrangementBar = addToolBar(tr("Arrangement"));
    arrangementBar->setObjectName(QStringLiteral("arrangementToolbar"));
    arrangementBar->setMovable(false);
    arrangementBar->addAction(addTrackAction);
    arrangementBar->addAction(addDrumTrackAction);
    if (addClapInstrumentTrackAction != nullptr)
        arrangementBar->addAction(addClapInstrumentTrackAction);
    arrangementBar->addAction(addPatternAction);
    arrangementBar->addSeparator();
    arrangementBar->addWidget(new QLabel(tr("CLIP LENGTH"), arrangementBar));
    clipLengthControl = new QDoubleSpinBox(arrangementBar);
    clipLengthControl->setRange(0.25, 16384.0);
    clipLengthControl->setDecimals(2);
    clipLengthControl->setSingleStep(1.0);
    clipLengthControl->setSuffix(tr(" beats"));
    clipLengthControl->setFixedWidth(118);
    clipLengthControl->setToolTip(tr("Length of the selected MIDI clip in quarter-note beats"));
    arrangementBar->addWidget(clipLengthControl);
    connect(clipLengthControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double length)
    {
        MidiClip* clip = selectedClip();
        if (clip == nullptr)
            return;
        clip->lengthBeats = length;
        project.normalize();
        const MidiClip* updatedClip = selectedClip();
        midiEditor->setPatternLengthBeats(updatedClip == nullptr ? project.patternLengthBeats() : updatedClip->lengthBeats);
        midiEditor->setNotes(updatedClip == nullptr ? QVector<MidiNote>{} : updatedClip->notes);
        midiEditor->setPitchBends(updatedClip == nullptr ? QVector<MidiPitchBend>{} : updatedClip->pitchBends);
        midiEditor->setPitchBendLaneEnabled(updatedClip != nullptr && !updatedClip->isAudioRegion());
        stepSequencer->setPattern(updatedClip == nullptr ? QVector<MidiNote>{} : updatedClip->notes,
                                  updatedClip == nullptr ? project.patternLengthBeats() : updatedClip->lengthBeats,
                                  project.quarterNotesPerBar(), project.nextNoteId(), updatedClip != nullptr,
                                  updatedClip == nullptr ? 16 : updatedClip->stepsPerBar);
        arrangementEditor->setProject(project);
        arrangementEditor->setSelection(selectedTrackId, selectedClipId);
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    arrangementBar->addSeparator();
    automationViewAction = new QAction(tr("Automation lanes"), this);
    automationViewAction->setCheckable(true);
    automationViewAction->setToolTip(tr("Show and edit volume, pan, or LV2/CLAP parameter automation in the Playlist"));
    arrangementBar->addAction(automationViewAction);
    automationLaneParameterControl = new QComboBox(arrangementBar);
    automationLaneParameterControl->addItem(tr("Volume"), 0);
    automationLaneParameterControl->addItem(tr("Pan"), 1);
    automationLaneParameterControl->setMinimumWidth(190);
    automationLaneParameterControl->setEnabled(false);
    automationLaneParameterControl->setToolTip(tr("Volume, pan, or LV2/CLAP parameter shown in the Playlist lanes"));
    arrangementBar->addWidget(automationLaneParameterControl);
    automationWriteModeControl = new QComboBox(arrangementBar);
    automationWriteModeControl->addItem(tr("Touch"), 0);
    automationWriteModeControl->addItem(tr("Latch"), 1);
    automationWriteModeControl->addItem(tr("Trim"), 2);
    automationWriteModeControl->setFixedWidth(82);
    automationWriteModeControl->setToolTip(tr("Touch writes while the Mixer control is held; Latch keeps writing its last value; Trim records control movement as an offset from existing automation"));
    arrangementBar->addWidget(automationWriteModeControl);
    automationWriteAction = new QAction(tr("Write automation"), this);
    automationWriteAction->setCheckable(true);
    automationWriteAction->setToolTip(tr("Record the selected track volume, pan, or LV2/CLAP parameter while the transport plays"));
    arrangementBar->addAction(automationWriteAction);
    automationWriteIdleTimer = new QTimer(this);
    automationWriteIdleTimer->setSingleShot(true);
    connect(automationWriteIdleTimer, &QTimer::timeout, this, &MainWindow::finishAutomationWrite);
    connect(automationViewAction, &QAction::toggled, this, [this](bool enabled)
    {
        automationLaneParameterControl->setEnabled(enabled || automationWriteAction->isChecked());
        refreshArrangementAutomationView();
        if (enabled)
            statusBar()->showMessage(tr("Click a lane to add a point, drag points to edit, and right-click or press Delete to remove one."), 6000);
    });
    connect(automationWriteAction, &QAction::toggled, this, [this](bool enabled)
    {
        automationLaneParameterControl->setEnabled(enabled || automationViewAction->isChecked());
        if (enabled)
        {
            const int mode = automationWriteModeControl->currentData().toInt();
            statusBar()->showMessage(mode == 1
                ? tr("Latch is armed. Move the selected Mixer control; its last value keeps writing until playback stops or Write is disarmed.")
                : mode == 2
                    ? tr("Trim is armed. Move the selected Mixer control to add an offset to its existing automation.")
                    : tr("Touch is armed. Start playback and move the selected Mixer control."), 6000);
        }
        else
            finishAutomationWrite();
    });
    connect(automationWriteModeControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int)
    {
        if (automationWriteSession.active)
            finishAutomationWrite();
        if (automationWriteAction->isChecked())
        {
            const int mode = automationWriteModeControl->currentData().toInt();
            statusBar()->showMessage(mode == 1
                ? tr("Latch is armed. Move the selected Mixer control; its last value keeps writing until playback stops or Write is disarmed.")
                : mode == 2
                    ? tr("Trim is armed. Move the selected Mixer control to add an offset to its existing automation.")
                    : tr("Touch is armed. Start playback and move the selected Mixer control."), 5000);
        }
    });
    connect(automationLaneParameterControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (automationWriteSession.active &&
            automationWriteSession.parameter != automationLaneParameterControl->itemData(index).toInt())
            finishAutomationWrite();
        refreshArrangementAutomationView();
    });

    auto* transportBar = addToolBar(tr("Transport"));
    transportBar->setObjectName(QStringLiteral("transportToolbar"));
    transportBar->setMovable(false);
    transportBar->addAction(playAction);
    transportBar->addAction(stopAction);
    transportBar->addAction(loopAction);
    transportBar->addSeparator();
    transportBar->addWidget(new QLabel(tr("TEMPO"), transportBar));
    tempoControl = new QDoubleSpinBox(transportBar);
    tempoControl->setRange(20.0, 300.0);
    tempoControl->setDecimals(1);
    tempoControl->setSingleStep(1.0);
    tempoControl->setSuffix(tr(" BPM"));
    tempoControl->setToolTip(tr("Tempo at the start of the project. Add later tempo changes from the Playlist ruler."));
    tempoControl->setValue(project.tempoBpm);
    tempoControl->setFixedWidth(104);
    transportBar->addWidget(tempoControl);
    connect(tempoControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value)
    {
        project.tempoBpm = value;
        project.rebuildTempoCache();
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });
    transportBar->addSeparator();
    transportBar->addWidget(new QLabel(tr("METER"), transportBar));
    timeSignatureControl = new QComboBox(transportBar);
    const QVector<QPair<int, int>> commonMeters = {
        {4, 4}, {3, 4}, {2, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}, {2, 2}, {3, 8}
    };
    for (const auto& meter : commonMeters)
        timeSignatureControl->addItem(QStringLiteral("%1/%2").arg(meter.first).arg(meter.second),
                                      QVariantList{meter.first, meter.second});
    timeSignatureControl->setFixedWidth(76);
    timeSignatureControl->setToolTip(tr("Arrangement time signature"));
    transportBar->addWidget(timeSignatureControl);
    connect(timeSignatureControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        if (index < 0)
            return;
        const QVariantList meter = timeSignatureControl->itemData(index).toList();
        if (meter.size() != 2)
            return;
        project.beatsPerBar = meter[0].toInt();
        project.beatUnit = meter[1].toInt();
        project.normalize();
        const MidiClip* clip = selectedClip();
        midiEditor->setTimeSignature(project.beatsPerBar, project.beatUnit);
        midiEditor->setPatternLengthBeats(clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats);
        stepSequencer->setPattern(clip == nullptr ? QVector<MidiNote>{} : clip->notes,
                                  clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats,
                                  project.quarterNotesPerBar(), project.nextNoteId(), clip != nullptr,
                                  clip == nullptr ? 16 : clip->stepsPerBar);
        arrangementEditor->setProject(project);
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });
    transportBar->addSeparator();
    transportBar->addAction(metronomeAction);
    transportBar->addWidget(new QLabel(tr("COUNT-IN"), transportBar));
    countInControl = new QComboBox(transportBar);
    countInControl->addItem(tr("Off"), 0);
    countInControl->addItem(tr("1 bar"), 1);
    countInControl->addItem(tr("2 bars"), 2);
    countInControl->addItem(tr("3 bars"), 3);
    countInControl->addItem(tr("4 bars"), 4);
    countInControl->setCurrentIndex(0);
    countInControl->setFixedWidth(78);
    countInControl->setToolTip(tr("Bars of metronome count-in before playback"));
    transportBar->addWidget(countInControl);
    connect(countInControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        if (index < 0)
            return;
        project.countInBars = countInControl->itemData(index).toInt();
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    auto* recordingBar = addToolBar(tr("Recording"));
    recordingBar->setObjectName(QStringLiteral("recordingToolbar"));
    recordingBar->setMovable(false);
    recordingBar->addAction(recordAction);
    recordingBar->addAction(audioMonitorAction);
    recordingBar->addSeparator();
    recordingBar->addWidget(new QLabel(tr("INPUT"), recordingBar));
    audioInputControl = new QComboBox(recordingBar);
    audioInputControl->setMinimumWidth(190);
    audioInputControl->setToolTip(tr("Audio device used for new recordings"));
    recordingBar->addWidget(audioInputControl);
    recordingBar->addSeparator();
    recordingBar->addAction(midiRecordAction);
    recordingBar->addAction(midiMonitorAction);
    recordingBar->addWidget(new QLabel(tr("MIDI INPUT"), recordingBar));
    midiInputControl = new QComboBox(recordingBar);
    midiInputControl->setMinimumWidth(190);
    midiInputControl->setToolTip(tr("MIDI device used for live monitoring and recording"));
    recordingBar->addWidget(midiInputControl);
    mediaDevices = new QMediaDevices(this);
    connect(mediaDevices, &QMediaDevices::audioInputsChanged,
            this, &MainWindow::refreshAudioInputs);
    refreshAudioInputs();
    midiInput = std::make_unique<MidiInput>();
    refreshMidiInputs();
    midiInputPollTimer = new QTimer(this);
    midiInputPollTimer->setInterval(5);
    connect(midiInputPollTimer, &QTimer::timeout, this, &MainWindow::pollMidiInput);

    auto* editorBar = addToolBar(tr("Piano roll"));
    editorBar->setObjectName(QStringLiteral("pianoRollToolbar"));
    editorBar->setMovable(false);
    editorBar->addWidget(new QLabel(tr("SNAP"), editorBar));
    snapControl = new QComboBox(editorBar);
    snapControl->addItem(tr("1/4"), 1.0);
    snapControl->addItem(tr("1/8"), 0.5);
    snapControl->addItem(tr("1/16"), 0.25);
    snapControl->addItem(tr("1/32"), 0.125);
    snapControl->addItem(tr("1/4T"), 2.0 / 3.0);
    snapControl->addItem(tr("1/8T"), 1.0 / 3.0);
    snapControl->addItem(tr("1/16T"), 1.0 / 6.0);
    snapControl->addItem(tr("1/32T"), 1.0 / 12.0);
    snapControl->addItem(tr("1/4."), 1.5);
    snapControl->addItem(tr("1/8."), 0.75);
    snapControl->addItem(tr("1/16."), 0.375);
    snapControl->addItem(tr("1/32."), 0.1875);
    snapControl->setCurrentIndex(2);
    snapControl->setToolTip(tr("Straight, triplet, or dotted piano-roll grid snap interval"));
    editorBar->addWidget(snapControl);
    connect(snapControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        midiEditor->setSnap(snapControl->itemData(index).toDouble());
    });
    editorBar->addSeparator();
    editorBar->addWidget(new QLabel(tr("KEY"), editorBar));
    pianoRollKeyControl = new QComboBox(editorBar);
    const QStringList pitchClassNames = {
        tr("C"), tr("C♯"), tr("D"), tr("D♯"), tr("E"), tr("F"),
        tr("F♯"), tr("G"), tr("G♯"), tr("A"), tr("A♯"), tr("B")
    };
    for (const QString& pitchClass : pitchClassNames)
        pianoRollKeyControl->addItem(pitchClass);
    pianoRollKeyControl->setFixedWidth(64);
    pianoRollKeyControl->setToolTip(tr("Root note used to highlight and snap the selected scale"));
    editorBar->addWidget(pianoRollKeyControl);
    editorBar->addWidget(new QLabel(tr("SCALE"), editorBar));
    pianoRollScaleControl = new QComboBox(editorBar);
    pianoRollScaleControl->addItem(tr("Major"), QVariantList{0, 2, 4, 5, 7, 9, 11});
    pianoRollScaleControl->addItem(tr("Natural minor"), QVariantList{0, 2, 3, 5, 7, 8, 10});
    pianoRollScaleControl->addItem(tr("Dorian"), QVariantList{0, 2, 3, 5, 7, 9, 10});
    pianoRollScaleControl->addItem(tr("Mixolydian"), QVariantList{0, 2, 4, 5, 7, 9, 10});
    pianoRollScaleControl->addItem(tr("Major pentatonic"), QVariantList{0, 2, 4, 7, 9});
    pianoRollScaleControl->addItem(tr("Minor pentatonic"), QVariantList{0, 3, 5, 7, 10});
    pianoRollScaleControl->addItem(tr("Chromatic"), QVariantList{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    pianoRollScaleControl->setFixedWidth(132);
    pianoRollScaleControl->setToolTip(tr("Scale used to highlight piano-roll pitches"));
    editorBar->addWidget(pianoRollScaleControl);
    pianoRollScaleSnapAction = new QAction(tr("Scale snap"), this);
    pianoRollScaleSnapAction->setCheckable(true);
    pianoRollScaleSnapAction->setToolTip(tr("Snap newly drawn notes and keyboard auditions to the selected scale; existing notes are unchanged"));
    editorBar->addAction(pianoRollScaleSnapAction);
    const auto applyPianoRollScale = [this]
    {
        QVector<int> intervals;
        for (const QVariant& value : pianoRollScaleControl->currentData().toList())
            intervals.push_back(value.toInt());
        midiEditor->setScale(pianoRollKeyControl->currentIndex(), intervals,
                             pianoRollScaleSnapAction->isChecked());
    };
    connect(pianoRollKeyControl, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [applyPianoRollScale](int) { applyPianoRollScale(); });
    connect(pianoRollScaleControl, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [applyPianoRollScale](int) { applyPianoRollScale(); });
    connect(pianoRollScaleSnapAction, &QAction::toggled,
            this, [applyPianoRollScale](bool) { applyPianoRollScale(); });
    applyPianoRollScale();
    pianoRollGhostNotesAction = new QAction(tr("Ghost notes"), this);
    pianoRollGhostNotesAction->setCheckable(true);
    pianoRollGhostNotesAction->setChecked(true);
    pianoRollGhostNotesAction->setToolTip(tr("Show notes from other MIDI clips on the selected track as non-editable guides"));
    connect(pianoRollGhostNotesAction, &QAction::toggled,
            midiEditor, &MidiEditor::setGhostNotesEnabled);
    editorBar->addAction(pianoRollGhostNotesAction);
    editorBar->addSeparator();
    editorBar->addWidget(new QLabel(tr("ZOOM"), editorBar));
    zoomControl = new QSlider(Qt::Horizontal, editorBar);
    zoomControl->setRange(48, 132);
    zoomControl->setValue(84);
    zoomControl->setFixedWidth(110);
    zoomControl->setToolTip(tr("Piano roll zoom"));
    editorBar->addWidget(zoomControl);
    connect(zoomControl, &QSlider::valueChanged, midiEditor, &MidiEditor::setPixelsPerBeat);
}

void MainWindow::createDocks()
{
    browserDock = new QDockWidget(tr("Browser"), this);
    browserDock->setObjectName(QStringLiteral("browserDock"));
    browserDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* browser = new QWidget(browserDock);
    auto* browserLayout = new QVBoxLayout(browser);
    browserLayout->setContentsMargins(10, 12, 10, 10);
    browserLayout->setSpacing(8);
    auto* trackHeading = new QLabel(tr("INSTRUMENT TRACKS"), browser);
    trackHeading->setStyleSheet(QStringLiteral("color: #8d9aa7; font-size: 10px; font-weight: 700; letter-spacing: 1px;"));
    browserLayout->addWidget(trackHeading);
    trackList = new QListWidget(browser);
    trackList->setMinimumHeight(160);
    browserLayout->addWidget(trackList);
    connect(trackList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current)
    {
        if (current != nullptr)
            selectTrack(current->data(Qt::UserRole).toULongLong());
    });

    auto* buttonRow = new QHBoxLayout;
    auto* addTrackButton = new QPushButton(tr("Add track"), browser);
    auto* addPatternButton = new QPushButton(tr("New pattern"), browser);
    connect(addTrackButton, &QPushButton::clicked, this, &MainWindow::addTrack);
    connect(addPatternButton, &QPushButton::clicked, this, &MainWindow::addNewPattern);
    buttonRow->addWidget(addTrackButton);
    buttonRow->addWidget(addPatternButton);
    browserLayout->addLayout(buttonRow);
    auto* addDrumButton = new QPushButton(tr("Add drum rack"), browser);
    connect(addDrumButton, &QPushButton::clicked, this, &MainWindow::addDrumTrack);
    browserLayout->addWidget(addDrumButton);
#if defined(AWESOME_STUDIO_HAS_CLAP)
    auto* addClapInstrumentButton = new QPushButton(tr("Add CLAP instrument"), browser);
    connect(addClapInstrumentButton, &QPushButton::clicked,
            this, &MainWindow::addClapInstrumentTrack);
    browserLayout->addWidget(addClapInstrumentButton);
#endif
    auto* importAudioButton = new QPushButton(tr("Import WAV audio"), browser);
    connect(importAudioButton, &QPushButton::clicked, this, &MainWindow::importAudio);
    browserLayout->addWidget(importAudioButton);

    instrumentHeading = new QLabel(tr("SELECTED INSTRUMENT"), browser);
    instrumentHeading->setStyleSheet(QStringLiteral("color: #8d9aa7; font-size: 10px; font-weight: 700; margin-top: 10px;"));
    browserLayout->addWidget(instrumentHeading);
    clapInstrumentControlsButton = new QPushButton(tr("Instrument controls…"), browser);
    clapInstrumentControlsButton->setToolTip(
        tr("Edit the selected CLAP instrument's advertised parameters"));
    clapInstrumentControlsButton->setVisible(false);
    connect(clapInstrumentControlsButton, &QPushButton::clicked,
            this, &MainWindow::editSelectedClapInstrument);
    browserLayout->addWidget(clapInstrumentControlsButton);
    synthPresetPanel = new QWidget(browser);
    auto* synthPresetRow = new QHBoxLayout(synthPresetPanel);
    synthPresetRow->setContentsMargins(0, 0, 0, 0);
    auto* synthPresetLabel = new QLabel(tr("Preset"), synthPresetPanel);
    synthPresetControl = new QComboBox(synthPresetPanel);
    synthPresetControl->setPlaceholderText(tr("Custom"));
    synthPresetControl->setToolTip(
        tr("Factory presets set oscillator, amplitude, and filter settings. Editing them makes a custom sound."));
    for (size_t index = 0; index < factorySynthPresets.size(); ++index)
        synthPresetControl->addItem(tr(factorySynthPresets[index].name),
                                    static_cast<int>(index));
    synthPresetRow->addWidget(synthPresetLabel);
    synthPresetRow->addWidget(synthPresetControl, 1);
    connect(synthPresetControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (index >= 0)
            applySelectedSynthPreset(synthPresetControl->itemData(index).toInt());
    });
    browserLayout->addWidget(synthPresetPanel);
    auto* oscillatorGroup = new QGroupBox(tr("OSCILLATORS"), browser);
    oscillatorPanel = oscillatorGroup;
    auto* oscillatorForm = new QFormLayout(oscillatorGroup);
    waveformControl = new QComboBox(oscillatorGroup);
    waveformControl->addItem(tr("Sine"), static_cast<int>(SynthWaveform::Sine));
    waveformControl->addItem(tr("Triangle"), static_cast<int>(SynthWaveform::Triangle));
    waveformControl->addItem(tr("Saw"), static_cast<int>(SynthWaveform::Saw));
    waveformControl->addItem(tr("Square"), static_cast<int>(SynthWaveform::Square));
    oscillatorForm->addRow(tr("Oscillator 1"), waveformControl);
    connect(waveformControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        InstrumentTrack* track = selectedTrack();
        if (track == nullptr) return;
        synthPresetControl->setCurrentIndex(-1);
        track->waveform = static_cast<SynthWaveform>(waveformControl->itemData(index).toInt());
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    secondOscillatorEnabledControl = new QCheckBox(tr("Enable second oscillator"), oscillatorGroup);
    oscillatorForm->addRow(secondOscillatorEnabledControl);
    secondOscillatorWaveformControl = new QComboBox(oscillatorGroup);
    secondOscillatorWaveformControl->addItem(tr("Sine"), static_cast<int>(SynthWaveform::Sine));
    secondOscillatorWaveformControl->addItem(tr("Triangle"), static_cast<int>(SynthWaveform::Triangle));
    secondOscillatorWaveformControl->addItem(tr("Saw"), static_cast<int>(SynthWaveform::Saw));
    secondOscillatorWaveformControl->addItem(tr("Square"), static_cast<int>(SynthWaveform::Square));
    oscillatorForm->addRow(tr("Oscillator 2 waveform"), secondOscillatorWaveformControl);
    secondOscillatorDetuneControl = new QDoubleSpinBox(oscillatorGroup);
    secondOscillatorDetuneControl->setRange(-100.0, 100.0);
    secondOscillatorDetuneControl->setDecimals(1);
    secondOscillatorDetuneControl->setSingleStep(1.0);
    secondOscillatorDetuneControl->setSuffix(tr(" cents"));
    oscillatorForm->addRow(tr("Oscillator 2 detune"), secondOscillatorDetuneControl);
    secondOscillatorMixControl = new QDoubleSpinBox(oscillatorGroup);
    secondOscillatorMixControl->setRange(0.0, 100.0);
    secondOscillatorMixControl->setDecimals(0);
    secondOscillatorMixControl->setSingleStep(5.0);
    secondOscillatorMixControl->setSuffix(QStringLiteral("%"));
    oscillatorForm->addRow(tr("Oscillator 2 mix"), secondOscillatorMixControl);
    connect(secondOscillatorEnabledControl, &QCheckBox::toggled, this,
            [this](bool enabled) { setSelectedSynthOscillator(0, enabled ? 1.0 : 0.0); });
    connect(secondOscillatorEnabledControl, &QCheckBox::toggled,
            secondOscillatorWaveformControl, &QWidget::setEnabled);
    connect(secondOscillatorEnabledControl, &QCheckBox::toggled,
            secondOscillatorDetuneControl, &QWidget::setEnabled);
    connect(secondOscillatorEnabledControl, &QCheckBox::toggled,
            secondOscillatorMixControl, &QWidget::setEnabled);
    connect(secondOscillatorWaveformControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (index >= 0)
            setSelectedSynthOscillator(1, secondOscillatorWaveformControl->itemData(index).toInt());
    });
    connect(secondOscillatorDetuneControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setSelectedSynthOscillator(2, value); });
    connect(secondOscillatorMixControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setSelectedSynthOscillator(3, value); });
    browserLayout->addWidget(oscillatorGroup);

    auto* synthFilterGroup = new QGroupBox(tr("SYNTH FILTER"), browser);
    synthFilterPanel = synthFilterGroup;
    auto* synthFilterForm = new QFormLayout(synthFilterGroup);
    synthFilterEnabledControl = new QCheckBox(tr("Enable filter"), synthFilterGroup);
    synthFilterForm->addRow(synthFilterEnabledControl);
    synthFilterTypeControl = new QComboBox(synthFilterGroup);
    synthFilterTypeControl->addItem(tr("Low-pass"), static_cast<int>(SynthFilterType::LowPass));
    synthFilterTypeControl->addItem(tr("High-pass"), static_cast<int>(SynthFilterType::HighPass));
    synthFilterTypeControl->addItem(tr("Band-pass"), static_cast<int>(SynthFilterType::BandPass));
    synthFilterTypeControl->addItem(tr("Notch"), static_cast<int>(SynthFilterType::Notch));
    synthFilterForm->addRow(tr("Type"), synthFilterTypeControl);
    synthFilterCutoffControl = new QDoubleSpinBox(synthFilterGroup);
    synthFilterCutoffControl->setRange(20.0, 20000.0);
    synthFilterCutoffControl->setDecimals(0);
    synthFilterCutoffControl->setSingleStep(100.0);
    synthFilterCutoffControl->setSuffix(tr(" Hz"));
    synthFilterForm->addRow(tr("Cutoff"), synthFilterCutoffControl);
    synthFilterResonanceControl = new QDoubleSpinBox(synthFilterGroup);
    synthFilterResonanceControl->setRange(0.1, 10.0);
    synthFilterResonanceControl->setDecimals(2);
    synthFilterResonanceControl->setSingleStep(0.1);
    synthFilterForm->addRow(tr("Resonance (Q)"), synthFilterResonanceControl);
    connect(synthFilterEnabledControl, &QCheckBox::toggled, this,
            [this](bool enabled) { setSelectedSynthFilter(0, enabled ? 1.0 : 0.0); });
    connect(synthFilterEnabledControl, &QCheckBox::toggled,
            synthFilterTypeControl, &QWidget::setEnabled);
    connect(synthFilterEnabledControl, &QCheckBox::toggled,
            synthFilterCutoffControl, &QWidget::setEnabled);
    connect(synthFilterEnabledControl, &QCheckBox::toggled,
            synthFilterResonanceControl, &QWidget::setEnabled);
    connect(synthFilterTypeControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (index >= 0)
            setSelectedSynthFilter(3, synthFilterTypeControl->itemData(index).toInt());
    });
    connect(synthFilterCutoffControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setSelectedSynthFilter(1, value); });
    connect(synthFilterResonanceControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double value) { setSelectedSynthFilter(2, value); });
    browserLayout->addWidget(synthFilterGroup);

    auto* synthFilterEnvelopeGroup = new QGroupBox(tr("FILTER ENVELOPE"), browser);
    synthFilterEnvelopePanel = synthFilterEnvelopeGroup;
    auto* synthFilterEnvelopeForm = new QFormLayout(synthFilterEnvelopeGroup);
    synthFilterEnvelopeEnabledControl = new QCheckBox(tr("Enable filter envelope"),
                                                       synthFilterEnvelopeGroup);
    synthFilterEnvelopeForm->addRow(synthFilterEnvelopeEnabledControl);
    const std::array<QString, 5> filterEnvelopeLabels = {
        tr("Amount"), tr("Attack (s)"), tr("Decay (s)"), tr("Sustain"), tr("Release (s)")
    };
    for (int parameter = 0; parameter < static_cast<int>(synthFilterEnvelopeControls.size());
         ++parameter)
    {
        QDoubleSpinBox* control = new QDoubleSpinBox(synthFilterEnvelopeGroup);
        if (parameter == 0)
        {
            control->setRange(-48.0, 48.0);
            control->setDecimals(1);
            control->setSingleStep(1.0);
            control->setSuffix(tr(" st"));
            control->setToolTip(tr("Positive values raise the cutoff as the envelope opens."));
        }
        else if (parameter == 4)
        {
            control->setRange(0.0, 5.0);
            control->setDecimals(3);
            control->setSingleStep(0.01);
            control->setSuffix(tr(" s"));
        }
        else if (parameter == 3)
        {
            control->setRange(0.0, 1.0);
            control->setDecimals(2);
            control->setSingleStep(0.05);
        }
        else
        {
            control->setRange(0.0, 5.0);
            control->setDecimals(3);
            control->setSingleStep(0.01);
            control->setSuffix(tr(" s"));
        }
        synthFilterEnvelopeControls[static_cast<size_t>(parameter)] = control;
        synthFilterEnvelopeForm->addRow(filterEnvelopeLabels[static_cast<size_t>(parameter)],
                                        control);
        connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, parameter](double value)
        {
            setSelectedSynthFilterEnvelope(parameter + 1, value);
        });
    }
    connect(synthFilterEnvelopeEnabledControl, &QCheckBox::toggled, this,
            [this](bool enabled)
    {
        setSelectedSynthFilterEnvelope(0, enabled ? 1.0 : 0.0);
        for (QDoubleSpinBox* control : synthFilterEnvelopeControls)
            control->setEnabled(enabled && synthFilterEnvelopePanel->isEnabled());
    });
    connect(synthFilterEnabledControl, &QCheckBox::toggled, this,
            [this](bool enabled)
    {
        synthFilterEnvelopePanel->setEnabled(enabled);
        for (QDoubleSpinBox* control : synthFilterEnvelopeControls)
            control->setEnabled(enabled && synthFilterEnvelopeEnabledControl->isChecked());
    });
    browserLayout->addWidget(synthFilterEnvelopeGroup);

    auto* synthEnvelopeGroup = new QGroupBox(tr("SYNTH ENVELOPE"), browser);
    synthEnvelopePanel = synthEnvelopeGroup;
    auto* synthEnvelopeForm = new QFormLayout(synthEnvelopeGroup);
    const std::array<QString, 4> envelopeLabels = {
        tr("Attack (s)"), tr("Decay (s)"), tr("Sustain"), tr("Release (s)")
    };
    for (int parameter = 0; parameter < static_cast<int>(envelopeControls.size()); ++parameter)
    {
        QDoubleSpinBox* control = new QDoubleSpinBox(synthEnvelopeGroup);
        control->setDecimals(parameter == 2 ? 2 : 3);
        control->setSingleStep(parameter == 2 ? 0.05 : 0.01);
        if (parameter != 2)
        {
            control->setRange(0.0, parameter == 3 ? 10.0 : 5.0);
            control->setSuffix(tr(" s"));
        }
        else
        {
            control->setRange(0.0, 1.0);
        }
        envelopeControls[static_cast<size_t>(parameter)] = control;
        synthEnvelopeForm->addRow(envelopeLabels[static_cast<size_t>(parameter)], control);
        connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, parameter](double value) { setSelectedSynthEnvelope(parameter, value); });
    }
    browserLayout->addWidget(synthEnvelopeGroup);

    auto* equalizerGroup = new QGroupBox(tr("TRACK EQUALIZER"), browser);
    equalizerControls = equalizerGroup;
    auto* equalizerForm = new QFormLayout(equalizerGroup);
    const std::array<QString, 3> equalizerLabels = {
        tr("Low band gain"), tr("Mid band gain"), tr("High band gain")
    };
    for (int band = 0; band < static_cast<int>(equalizerGainControls.size()); ++band)
    {
        QDoubleSpinBox* control = new QDoubleSpinBox(equalizerGroup);
        control->setRange(-18.0, 18.0);
        control->setDecimals(1);
        control->setSingleStep(0.5);
        control->setSuffix(tr(" dB"));
        equalizerGainControls[static_cast<size_t>(band)] = control;
        equalizerForm->addRow(equalizerLabels[static_cast<size_t>(band)], control);
        connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, band](double value) { setSelectedTrackEqualizer(band, value); });
    }
    const std::array<QString, 2> crossoverLabels = {
        tr("Low / mid split"), tr("Mid / high split")
    };
    for (int band = 0; band < static_cast<int>(equalizerCrossoverControls.size()); ++band)
    {
        QDoubleSpinBox* control = new QDoubleSpinBox(equalizerGroup);
        control->setDecimals(0);
        control->setSingleStep(band == 0 ? 10.0 : 100.0);
        control->setSuffix(tr(" Hz"));
        control->setRange(band == 0 ? 20.0 : 500.0, band == 0 ? 2000.0 : 20000.0);
        equalizerCrossoverControls[static_cast<size_t>(band)] = control;
        equalizerForm->addRow(crossoverLabels[static_cast<size_t>(band)], control);
        connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, band](double value) { setSelectedTrackEqualizerCrossover(band, value); });
    }

    auto* parametricEqGroup = new QGroupBox(tr("PARAMETRIC BANDS"), equalizerGroup);
    auto* parametricEqLayout = new QVBoxLayout(parametricEqGroup);
    auto* responseGraph = new TrackEqResponseWidget(parametricEqGroup);
    equalizerResponseGraph = responseGraph;
    parametricEqLayout->addWidget(responseGraph);
    parametricEqBandControl = new QComboBox(parametricEqGroup);
    for (int band = 0; band < 3; ++band)
        parametricEqBandControl->addItem(tr("Bell %1").arg(band + 1), band);
    parametricEqLayout->addWidget(parametricEqBandControl);
    auto* parametricEqForm = new QFormLayout;
    const std::array<QString, 3> parametricLabels = {
        tr("Frequency"), tr("Gain"), tr("Q")
    };
    for (int parameter = 0; parameter < static_cast<int>(parametricEqBandParameterControls.size());
         ++parameter)
    {
        QDoubleSpinBox* control = new QDoubleSpinBox(parametricEqGroup);
        if (parameter == 0)
        {
            control->setRange(20.0, 20000.0);
            control->setDecimals(0);
            control->setSingleStep(10.0);
            control->setSuffix(tr(" Hz"));
        }
        else if (parameter == 1)
        {
            control->setRange(-18.0, 18.0);
            control->setDecimals(1);
            control->setSingleStep(0.5);
            control->setSuffix(tr(" dB"));
        }
        else
        {
            control->setRange(0.1, 10.0);
            control->setDecimals(2);
            control->setSingleStep(0.1);
        }
        parametricEqBandParameterControls[static_cast<size_t>(parameter)] = control;
        parametricEqForm->addRow(parametricLabels[static_cast<size_t>(parameter)], control);
        connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, parameter](double value)
        {
            InstrumentTrack* track = selectedTrack();
            const int band = parametricEqBandControl->currentData().toInt();
            if (track == nullptr || band < 0 || band >= static_cast<int>(track->eqParametricBands.size()))
                return;
            ParametricEqBand settings = track->eqParametricBands[static_cast<size_t>(band)];
            if (parameter == 0) settings.frequencyHz = static_cast<float>(value);
            else if (parameter == 1) settings.gainDb = static_cast<float>(value);
            else settings.q = static_cast<float>(value);
            setSelectedTrackParametricEqualizer(band, settings);
        });
    }
    parametricEqLayout->addLayout(parametricEqForm);
    equalizerForm->addRow(parametricEqGroup);
    connect(parametricEqBandControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { refreshParametricEqualizerControls(); });
    responseGraph->setSelectionHandler([this](int band)
    {
        const QSignalBlocker bandBlocker(parametricEqBandControl);
        parametricEqBandControl->setCurrentIndex(band);
        refreshParametricEqualizerControls();
    });
    responseGraph->setChangeHandler([this](int band, float frequencyHz, float gainDb)
    {
        InstrumentTrack* track = selectedTrack();
        if (track == nullptr || band < 0 || band >= static_cast<int>(track->eqParametricBands.size()))
            return;
        ParametricEqBand settings = track->eqParametricBands[static_cast<size_t>(band)];
        settings.frequencyHz = frequencyHz;
        settings.gainDb = gainDb;
        setSelectedTrackParametricEqualizer(band, settings);
    });
    browserLayout->addWidget(equalizerGroup);

    auto* automationGroup = new QGroupBox(tr("TRACK AUTOMATION"), browser);
    automationEditorPanel = automationGroup;
    auto* automationLayout = new QVBoxLayout(automationGroup);
    automationParameterControl = new QComboBox(automationGroup);
    automationParameterControl->addItem(tr("Volume"), 0);
    automationParameterControl->addItem(tr("Pan"), 1);
    automationLayout->addWidget(automationParameterControl);
    automationPointsList = new QListWidget(automationGroup);
    automationPointsList->setMaximumHeight(110);
    automationLayout->addWidget(automationPointsList);
    auto* automationForm = new QFormLayout;
    automationBeatControl = new QDoubleSpinBox(automationGroup);
    automationBeatControl->setRange(0.0, 16384.0);
    automationBeatControl->setDecimals(2);
    automationBeatControl->setSingleStep(1.0);
    automationBeatControl->setSuffix(tr(" beats"));
    automationValueControl = new QDoubleSpinBox(automationGroup);
    automationValueControl->setRange(0.0, 100.0);
    automationValueControl->setDecimals(0);
    automationValueControl->setSingleStep(5.0);
    automationValueControl->setSuffix(tr("%"));
    automationValueLabel = new QLabel(tr("Volume (%)"), automationGroup);
    automationCurveControl = new QComboBox(automationGroup);
    automationCurveControl->addItem(tr("Linear"), static_cast<int>(AutomationPoint::Curve::Linear));
    automationCurveControl->addItem(tr("Smooth"), static_cast<int>(AutomationPoint::Curve::Smooth));
    automationCurveControl->addItem(tr("Ease In"), static_cast<int>(AutomationPoint::Curve::EaseIn));
    automationCurveControl->addItem(tr("Ease Out"), static_cast<int>(AutomationPoint::Curve::EaseOut));
    automationCurveControl->addItem(tr("Hold"), static_cast<int>(AutomationPoint::Curve::Hold));
    automationCurveControl->setToolTip(tr("Shape from this keyframe to the next one"));
    automationForm->addRow(tr("Position"), automationBeatControl);
    automationForm->addRow(automationValueLabel, automationValueControl);
    automationForm->addRow(tr("To next keyframe"), automationCurveControl);
    automationLayout->addLayout(automationForm);
    auto* automationButtons = new QHBoxLayout;
    auto* addAutomationButton = new QPushButton(tr("Add / replace keyframe"), automationGroup);
    removeAutomationPointButton = new QPushButton(tr("Remove"), automationGroup);
    automationButtons->addWidget(addAutomationButton, 1);
    automationButtons->addWidget(removeAutomationPointButton);
    automationLayout->addLayout(automationButtons);
    browserLayout->addWidget(automationGroup);
    connect(automationParameterControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refreshAutomationEditor(); });
    connect(automationPointsList, &QListWidget::currentRowChanged, this, [this](int row)
    {
        const QListWidgetItem* item = automationPointsList->item(row);
        if (item == nullptr)
        {
            removeAutomationPointButton->setEnabled(false);
            return;
        }
        const QSignalBlocker beatBlocker(automationBeatControl);
        const QSignalBlocker valueBlocker(automationValueControl);
        const QSignalBlocker curveBlocker(automationCurveControl);
        automationBeatControl->setValue(item->data(Qt::UserRole).toDouble());
        automationValueControl->setValue(item->data(Qt::UserRole + 1).toDouble());
        automationCurveControl->setCurrentIndex(automationCurveControl->findData(
            item->data(Qt::UserRole + 2).toInt()));
        removeAutomationPointButton->setEnabled(true);
    });
    connect(automationCurveControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int curve) { setSelectedAutomationCurve(curve); });
    connect(addAutomationButton, &QPushButton::clicked, this, &MainWindow::addAutomationPoint);
    connect(removeAutomationPointButton, &QPushButton::clicked, this, &MainWindow::removeAutomationPoint);

    audioRegionControls = new QGroupBox(tr("AUDIO REGION"), browser);
    auto* audioFadeForm = new QFormLayout(audioRegionControls);
    audioFadeInControl = new QDoubleSpinBox(audioRegionControls);
    audioFadeOutControl = new QDoubleSpinBox(audioRegionControls);
    for (QDoubleSpinBox* control : {audioFadeInControl, audioFadeOutControl})
    {
        control->setRange(0.0, 16384.0);
        control->setDecimals(2);
        control->setSingleStep(0.25);
        control->setSuffix(tr(" beats"));
    }
    audioFadeForm->addRow(tr("Fade in"), audioFadeInControl);
    audioFadeForm->addRow(tr("Fade out"), audioFadeOutControl);
    const auto makeFadeCurveControl = [this]
    {
        auto* control = new QComboBox(audioRegionControls);
        control->addItem(QObject::tr("Linear"), static_cast<int>(AudioFadeCurve::Linear));
        control->addItem(QObject::tr("Smooth"), static_cast<int>(AudioFadeCurve::Smooth));
        control->addItem(QObject::tr("Equal power"), static_cast<int>(AudioFadeCurve::EqualPower));
        control->addItem(QObject::tr("Custom power"), static_cast<int>(AudioFadeCurve::Custom));
        return control;
    };
    audioFadeInCurveControl = makeFadeCurveControl();
    audioFadeOutCurveControl = makeFadeCurveControl();
    audioFadeForm->addRow(tr("Fade-in curve"), audioFadeInCurveControl);
    audioFadeForm->addRow(tr("Fade-out curve"), audioFadeOutCurveControl);
    const auto makeFadeShapeControl = [this]
    {
        auto* control = new QDoubleSpinBox(audioRegionControls);
        control->setRange(0.25, 4.0);
        control->setDecimals(2);
        control->setSingleStep(0.25);
        control->setToolTip(tr("For Custom power, 1.00 is linear; lower values transition earlier and higher values delay the transition."));
        return control;
    };
    audioFadeInShapeControl = makeFadeShapeControl();
    audioFadeOutShapeControl = makeFadeShapeControl();
    audioFadeForm->addRow(tr("Fade-in custom shape"), audioFadeInShapeControl);
    audioFadeForm->addRow(tr("Fade-out custom shape"), audioFadeOutShapeControl);
    audioCrossfadeButton = new QPushButton(tr("Create crossfade with overlapping region"), audioRegionControls);
    audioFadeForm->addRow(audioCrossfadeButton);
    browserLayout->addWidget(audioRegionControls);
    audioRegionControls->hide();
    connect(audioFadeInControl, &QDoubleSpinBox::valueChanged, this,
            [this](double beats) { setSelectedAudioFade(true, beats); });
    connect(audioFadeOutControl, &QDoubleSpinBox::valueChanged, this,
            [this](double beats) { setSelectedAudioFade(false, beats); });
    connect(audioFadeInCurveControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (index >= 0)
        {
            setSelectedAudioFadeCurve(true, audioFadeInCurveControl->currentData().toInt());
            audioFadeInShapeControl->setEnabled(audioFadeInCurveControl->currentData().toInt() ==
                                                static_cast<int>(AudioFadeCurve::Custom));
        }
    });
    connect(audioFadeOutCurveControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (index >= 0)
        {
            setSelectedAudioFadeCurve(false, audioFadeOutCurveControl->currentData().toInt());
            audioFadeOutShapeControl->setEnabled(audioFadeOutCurveControl->currentData().toInt() ==
                                                 static_cast<int>(AudioFadeCurve::Custom));
        }
    });
    connect(audioFadeInShapeControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double shape) { setSelectedAudioFadeShape(true, shape); });
    connect(audioFadeOutShapeControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double shape) { setSelectedAudioFadeShape(false, shape); });
    connect(audioCrossfadeButton, &QPushButton::clicked,
            this, &MainWindow::createSelectedAudioCrossfade);

    drumSampleControls = new QGroupBox(tr("DRUM RACK PADS"), browser);
    auto* drumSampleLayout = new QVBoxLayout(drumSampleControls);
    const std::array<QString, 4> drumPadNames = {
        tr("Kick"), tr("Snare"), tr("Closed hat"), tr("Open hat")
    };
    for (int pad = 0; pad < static_cast<int>(drumPadNames.size()); ++pad)
    {
        auto* row = new QWidget(drumSampleControls);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        drumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Load %1 sample…").arg(drumPadNames[static_cast<size_t>(pad)]), row);
        clearDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(tr("Clear base layer"), row);
        rowLayout->addWidget(drumSampleButtons[static_cast<size_t>(pad)], 1);
        rowLayout->addWidget(clearDrumSampleButtons[static_cast<size_t>(pad)]);
        drumSampleLayout->addWidget(row);
        auto* veryLowVelocityRow = new QWidget(drumSampleControls);
        auto* veryLowVelocityRowLayout = new QHBoxLayout(veryLowVelocityRow);
        veryLowVelocityRowLayout->setContentsMargins(0, 0, 0, 0);
        veryLowVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Load %1 extra-low layer…").arg(drumPadNames[static_cast<size_t>(pad)]),
            veryLowVelocityRow);
        clearVeryLowVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Clear layer"), veryLowVelocityRow);
        veryLowVelocityRowLayout->addWidget(
            veryLowVelocityDrumSampleButtons[static_cast<size_t>(pad)], 1);
        veryLowVelocityRowLayout->addWidget(
            clearVeryLowVelocityDrumSampleButtons[static_cast<size_t>(pad)]);
        drumSampleLayout->addWidget(veryLowVelocityRow);
        auto* lowVelocityRow = new QWidget(drumSampleControls);
        auto* lowVelocityRowLayout = new QHBoxLayout(lowVelocityRow);
        lowVelocityRowLayout->setContentsMargins(0, 0, 0, 0);
        lowVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Load %1 low-velocity layer…").arg(drumPadNames[static_cast<size_t>(pad)]),
            lowVelocityRow);
        clearLowVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Clear layer"), lowVelocityRow);
        lowVelocityRowLayout->addWidget(lowVelocityDrumSampleButtons[static_cast<size_t>(pad)], 1);
        lowVelocityRowLayout->addWidget(clearLowVelocityDrumSampleButtons[static_cast<size_t>(pad)]);
        drumSampleLayout->addWidget(lowVelocityRow);
        auto* highVelocityRow = new QWidget(drumSampleControls);
        auto* highVelocityRowLayout = new QHBoxLayout(highVelocityRow);
        highVelocityRowLayout->setContentsMargins(0, 0, 0, 0);
        highVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Load %1 high-velocity layer…").arg(drumPadNames[static_cast<size_t>(pad)]),
            highVelocityRow);
        clearHighVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Clear layer"), highVelocityRow);
        highVelocityRowLayout->addWidget(highVelocityDrumSampleButtons[static_cast<size_t>(pad)], 1);
        highVelocityRowLayout->addWidget(clearHighVelocityDrumSampleButtons[static_cast<size_t>(pad)]);
        drumSampleLayout->addWidget(highVelocityRow);
        auto* veryHighVelocityRow = new QWidget(drumSampleControls);
        auto* veryHighVelocityRowLayout = new QHBoxLayout(veryHighVelocityRow);
        veryHighVelocityRowLayout->setContentsMargins(0, 0, 0, 0);
        veryHighVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Load %1 extra-high layer…").arg(drumPadNames[static_cast<size_t>(pad)]),
            veryHighVelocityRow);
        clearVeryHighVelocityDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Clear layer"), veryHighVelocityRow);
        veryHighVelocityRowLayout->addWidget(
            veryHighVelocityDrumSampleButtons[static_cast<size_t>(pad)], 1);
        veryHighVelocityRowLayout->addWidget(
            clearVeryHighVelocityDrumSampleButtons[static_cast<size_t>(pad)]);
        drumSampleLayout->addWidget(veryHighVelocityRow);
        auto* parameterForm = new QFormLayout();
        parameterForm->setContentsMargins(0, 0, 0, 0);
        drumLayerSettingsControls[static_cast<size_t>(pad)] =
            new QComboBox(drumSampleControls);
        drumLayerSettingsControls[static_cast<size_t>(pad)]->addItem(tr("Shared pad / synth voice"), -1);
        drumLayerSettingsControls[static_cast<size_t>(pad)]->addItem(tr("Very-low sample layer"), 0);
        drumLayerSettingsControls[static_cast<size_t>(pad)]->addItem(tr("Low sample layer"), 1);
        drumLayerSettingsControls[static_cast<size_t>(pad)]->addItem(tr("Base sample layer"), 2);
        drumLayerSettingsControls[static_cast<size_t>(pad)]->addItem(tr("High sample layer"), 3);
        drumLayerSettingsControls[static_cast<size_t>(pad)]->addItem(tr("Very-high sample layer"), 4);
        parameterForm->addRow(tr("Edit settings for"),
                              drumLayerSettingsControls[static_cast<size_t>(pad)]);
        resetDrumLayerSettingsButtons[static_cast<size_t>(pad)] =
            new QPushButton(tr("Reset layer to shared settings"), drumSampleControls);
        parameterForm->addRow(resetDrumLayerSettingsButtons[static_cast<size_t>(pad)]);
        drumSampleTuneControls[static_cast<size_t>(pad)] = new QDoubleSpinBox(drumSampleControls);
        drumSampleTuneControls[static_cast<size_t>(pad)]->setRange(-24.0, 24.0);
        drumSampleTuneControls[static_cast<size_t>(pad)]->setDecimals(1);
        drumSampleTuneControls[static_cast<size_t>(pad)]->setSingleStep(1.0);
        drumSampleTuneControls[static_cast<size_t>(pad)]->setSuffix(tr(" semitones"));
        parameterForm->addRow(tr("%1 tune").arg(drumPadNames[static_cast<size_t>(pad)]),
                              drumSampleTuneControls[static_cast<size_t>(pad)]);
        drumSampleStartControls[static_cast<size_t>(pad)] = new QDoubleSpinBox(drumSampleControls);
        drumSampleStartControls[static_cast<size_t>(pad)]->setRange(0.0, 99.0);
        drumSampleStartControls[static_cast<size_t>(pad)]->setDecimals(0);
        drumSampleStartControls[static_cast<size_t>(pad)]->setSingleStep(1.0);
        drumSampleStartControls[static_cast<size_t>(pad)]->setSuffix(QStringLiteral("%"));
        parameterForm->addRow(tr("%1 sample start").arg(drumPadNames[static_cast<size_t>(pad)]),
                              drumSampleStartControls[static_cast<size_t>(pad)]);
        drumSampleLoopControls[static_cast<size_t>(pad)] = new QCheckBox(
            tr("%1 loop while note is held").arg(drumPadNames[static_cast<size_t>(pad)]),
            drumSampleControls);
        parameterForm->addRow(drumSampleLoopControls[static_cast<size_t>(pad)]);
        drumPadAttackControls[static_cast<size_t>(pad)] = new QDoubleSpinBox(drumSampleControls);
        drumPadDecayControls[static_cast<size_t>(pad)] = new QDoubleSpinBox(drumSampleControls);
        drumPadSustainControls[static_cast<size_t>(pad)] = new QDoubleSpinBox(drumSampleControls);
        drumPadReleaseControls[static_cast<size_t>(pad)] = new QDoubleSpinBox(drumSampleControls);
        for (QDoubleSpinBox* control : {drumPadAttackControls[static_cast<size_t>(pad)],
                                        drumPadDecayControls[static_cast<size_t>(pad)],
                                        drumPadSustainControls[static_cast<size_t>(pad)],
                                        drumPadReleaseControls[static_cast<size_t>(pad)]})
        {
            control->setDecimals(3);
            control->setSuffix(tr(" s"));
            control->setSingleStep(0.01);
        }
        drumPadAttackControls[static_cast<size_t>(pad)]->setRange(0.0, 5.0);
        drumPadDecayControls[static_cast<size_t>(pad)]->setRange(0.0, 5.0);
        drumPadSustainControls[static_cast<size_t>(pad)]->setRange(0.0, 1.0);
        drumPadSustainControls[static_cast<size_t>(pad)]->setSuffix(QString());
        drumPadSustainControls[static_cast<size_t>(pad)]->setSingleStep(0.05);
        drumPadReleaseControls[static_cast<size_t>(pad)]->setRange(0.0, 5.0);
        drumPadAttackControls[static_cast<size_t>(pad)]->setToolTip(tr("Time to reach full level"));
        drumPadDecayControls[static_cast<size_t>(pad)]->setToolTip(tr("Time to reach the sustain level"));
        drumPadSustainControls[static_cast<size_t>(pad)]->setToolTip(tr("Level held after the decay stage"));
        drumPadReleaseControls[static_cast<size_t>(pad)]->setToolTip(tr("Fade time after the pad note ends"));
        parameterForm->addRow(tr("Pad attack"), drumPadAttackControls[static_cast<size_t>(pad)]);
        parameterForm->addRow(tr("Pad decay"), drumPadDecayControls[static_cast<size_t>(pad)]);
        parameterForm->addRow(tr("Pad sustain"), drumPadSustainControls[static_cast<size_t>(pad)]);
        parameterForm->addRow(tr("Pad release"), drumPadReleaseControls[static_cast<size_t>(pad)]);
        veryLowVelocityThresholdControls[static_cast<size_t>(pad)] =
            new QSpinBox(drumSampleControls);
        veryLowVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(1, 124);
        veryLowVelocityThresholdControls[static_cast<size_t>(pad)]->setSuffix(tr(" velocity"));
        parameterForm->addRow(tr("Extra-low layer below"),
                              veryLowVelocityThresholdControls[static_cast<size_t>(pad)]);
        lowVelocityThresholdControls[static_cast<size_t>(pad)] = new QSpinBox(drumSampleControls);
        lowVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(2, 125);
        lowVelocityThresholdControls[static_cast<size_t>(pad)]->setSuffix(tr(" velocity"));
        parameterForm->addRow(tr("Use low-velocity layer below"),
                              lowVelocityThresholdControls[static_cast<size_t>(pad)]);
        highVelocityThresholdControls[static_cast<size_t>(pad)] = new QSpinBox(drumSampleControls);
        highVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(3, 126);
        highVelocityThresholdControls[static_cast<size_t>(pad)]->setSuffix(tr(" velocity"));
        parameterForm->addRow(tr("Use high-velocity layer at"),
                              highVelocityThresholdControls[static_cast<size_t>(pad)]);
        veryHighVelocityThresholdControls[static_cast<size_t>(pad)] =
            new QSpinBox(drumSampleControls);
        veryHighVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(4, 127);
        veryHighVelocityThresholdControls[static_cast<size_t>(pad)]->setSuffix(tr(" velocity"));
        parameterForm->addRow(tr("Extra-high layer at"),
                              veryHighVelocityThresholdControls[static_cast<size_t>(pad)]);
        drumSampleLayout->addLayout(parameterForm);
        connect(drumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { importDrumSample(pad); });
        connect(clearDrumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { clearDrumSample(pad); });
        connect(veryLowVelocityDrumSampleButtons[static_cast<size_t>(pad)],
                &QPushButton::clicked, this,
                [this, pad] { importVeryLowVelocityDrumSample(pad); });
        connect(clearVeryLowVelocityDrumSampleButtons[static_cast<size_t>(pad)],
                &QPushButton::clicked, this,
                [this, pad] { clearVeryLowVelocityDrumSample(pad); });
        connect(lowVelocityDrumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { importLowVelocityDrumSample(pad); });
        connect(clearLowVelocityDrumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { clearLowVelocityDrumSample(pad); });
        connect(highVelocityDrumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { importHighVelocityDrumSample(pad); });
        connect(clearHighVelocityDrumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { clearHighVelocityDrumSample(pad); });
        connect(veryHighVelocityDrumSampleButtons[static_cast<size_t>(pad)],
                &QPushButton::clicked, this,
                [this, pad] { importVeryHighVelocityDrumSample(pad); });
        connect(clearVeryHighVelocityDrumSampleButtons[static_cast<size_t>(pad)],
                &QPushButton::clicked, this,
                [this, pad] { clearVeryHighVelocityDrumSample(pad); });
        connect(drumLayerSettingsControls[static_cast<size_t>(pad)],
                qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { refreshProjectUi(); });
        connect(resetDrumLayerSettingsButtons[static_cast<size_t>(pad)],
                &QPushButton::clicked, this,
                [this, pad] { resetSelectedDrumLayerSettings(pad); });
        connect(drumSampleTuneControls[static_cast<size_t>(pad)],
                qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, pad](double value) { setSelectedDrumSample(pad, 0, value); });
        connect(drumSampleStartControls[static_cast<size_t>(pad)],
                qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, pad](double value) { setSelectedDrumSample(pad, 1, value); });
        connect(drumSampleLoopControls[static_cast<size_t>(pad)], &QCheckBox::toggled, this,
                [this, pad](bool enabled) { setSelectedDrumSample(pad, 2, enabled ? 1.0 : 0.0); });
        connect(drumPadAttackControls[static_cast<size_t>(pad)],
                qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, pad](double value) { setSelectedDrumSample(pad, 4, value); });
        connect(drumPadDecayControls[static_cast<size_t>(pad)],
                qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, pad](double value) { setSelectedDrumSample(pad, 5, value); });
        connect(drumPadSustainControls[static_cast<size_t>(pad)],
                qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, pad](double value) { setSelectedDrumSample(pad, 6, value); });
        connect(drumPadReleaseControls[static_cast<size_t>(pad)],
                qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, pad](double value) { setSelectedDrumSample(pad, 7, value); });
        connect(highVelocityThresholdControls[static_cast<size_t>(pad)], qOverload<int>(&QSpinBox::valueChanged),
                this, [this, pad](int value) { setSelectedDrumSample(pad, 3, value); });
        connect(lowVelocityThresholdControls[static_cast<size_t>(pad)], qOverload<int>(&QSpinBox::valueChanged),
                this, [this, pad](int value) { setSelectedDrumSample(pad, 8, value); });
        connect(veryLowVelocityThresholdControls[static_cast<size_t>(pad)],
                qOverload<int>(&QSpinBox::valueChanged), this,
                [this, pad](int value) { setSelectedDrumSample(pad, 9, value); });
        connect(veryHighVelocityThresholdControls[static_cast<size_t>(pad)],
                qOverload<int>(&QSpinBox::valueChanged), this,
                [this, pad](int value) { setSelectedDrumSample(pad, 10, value); });
    }
    browserLayout->addWidget(drumSampleControls);
    drumSampleControls->hide();

    browserLayout->addStretch();
    auto* hint = new QLabel(tr("Import a WAV to add an audio track. Double-click MIDI clips to edit notes or drum steps."), browser);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8996a2; font-size: 11px;"));
    browserLayout->addWidget(hint);
    auto* browserScrollArea = new QScrollArea(browserDock);
    browserScrollArea->setWidgetResizable(true);
    browserScrollArea->setFrameShape(QFrame::NoFrame);
    browserScrollArea->setWidget(browser);
    browserDock->setWidget(browserScrollArea);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);

    mixerDock = new QDockWidget(tr("Mixer"), this);
    mixerDock->setObjectName(QStringLiteral("mixerDock"));
    mixerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    mixerScrollArea = new QScrollArea(mixerDock);
    mixerScrollArea->setWidgetResizable(true);
    mixerScrollArea->setFrameShape(QFrame::NoFrame);
    mixerDock->setWidget(mixerScrollArea);
    addDockWidget(Qt::RightDockWidgetArea, mixerDock);

    audioDeviceLabel = new QLabel(this);
    audioDeviceLabel->setContentsMargins(8, 0, 8, 0);
    playheadLabel = new QLabel(tr("BAR 01  ·  BEAT 1.00"), this);
    playheadLabel->setContentsMargins(8, 0, 8, 0);
    playheadLabel->setStyleSheet(QStringLiteral("font-variant-numeric: tabular-nums; color: #d8e0e8;"));
    statusBar()->addPermanentWidget(playheadLabel);
    statusBar()->addPermanentWidget(audioDeviceLabel);
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    for (QAction* action : findChildren<QAction*>())
    {
        if (action->text() == tr("New") || action->text() == tr("Open") || action->text() == tr("Save"))
            fileMenu->addAction(action);
    }
    auto* saveAsAction = fileMenu->addAction(tr("Save project as…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this]
    {
        QString path = QFileDialog::getSaveFileName(this, tr("Save project as"), projectPath,
                                                     tr("Awesome Studio project (*.aws)"));
        if (!path.isEmpty())
        {
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".aws");
            saveToPath(path);
        }
    });
    fileMenu->addAction(tr("Save portable project copy…"), this,
                        &MainWindow::savePortableProjectCopy);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Import WAV audio…"), this, &MainWindow::importAudio);
    fileMenu->addAction(tr("Import MIDI…"), this, &MainWindow::importMidi);
    fileMenu->addAction(tr("Export arrangement to MIDI…"), this, &MainWindow::exportMidi);
    fileMenu->addAction(tr("Export arrangement to WAV…"), this, &MainWindow::exportAudio);
    fileMenu->addAction(tr("Export track stems…"), this, &MainWindow::exportStems);
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(tr("Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(undoAction);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    editMenu->addAction(tr("Note properties…"), midiEditor, &MidiEditor::editSelectedNoteProperties);
    editMenu->addAction(tr("Quantize note starts to grid"), midiEditor, &MidiEditor::quantizeNotesToGrid);
    QMenu* chordMenu = editMenu->addMenu(tr("Build chord from selection"));
    const auto addChordAction = [this, chordMenu](const QString& label, const QVector<int>& intervals)
    {
        QAction* action = chordMenu->addAction(label);
        connect(action, &QAction::triggered, this, [this, label, intervals]
        {
            if (midiEditor->addChordToSelection(intervals))
                statusBar()->showMessage(tr("Applied %1 to the selected root notes").arg(label), 1800);
            else
                statusBar()->showMessage(tr("Select one or more root notes in the piano roll first"), 2400);
        });
    };
    addChordAction(tr("Major triad"), {0, 4, 7});
    addChordAction(tr("Minor triad"), {0, 3, 7});
    addChordAction(tr("Diminished triad"), {0, 3, 6});
    addChordAction(tr("Augmented triad"), {0, 4, 8});
    addChordAction(tr("Suspended 2nd"), {0, 2, 7});
    addChordAction(tr("Suspended 4th"), {0, 5, 7});
    addChordAction(tr("Dominant 7th"), {0, 4, 7, 10});
    addChordAction(tr("Major 7th"), {0, 4, 7, 11});
    addChordAction(tr("Minor 7th"), {0, 3, 7, 10});
    QAction* arpeggiateAction = editMenu->addAction(tr("Arpeggiate selected chord…"));
    connect(arpeggiateAction, &QAction::triggered, this, [this]
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Arpeggiator"));
        auto* form = new QFormLayout(&dialog);
        auto* directionControl = new QComboBox(&dialog);
        directionControl->addItem(tr("Up"), 0);
        directionControl->addItem(tr("Down"), 1);
        directionControl->addItem(tr("Up and down"), 2);
        form->addRow(tr("Direction"), directionControl);

        auto* octaveControl = new QSpinBox(&dialog);
        octaveControl->setRange(1, 4);
        octaveControl->setValue(1);
        octaveControl->setSuffix(tr(" octaves"));
        form->addRow(tr("Range"), octaveControl);

        auto* gateControl = new QSpinBox(&dialog);
        gateControl->setRange(10, 100);
        gateControl->setValue(90);
        gateControl->setSuffix(QStringLiteral("%"));
        form->addRow(tr("Gate"), gateControl);

        auto* velocityPatternControl = new QComboBox(&dialog);
        velocityPatternControl->addItem(tr("Keep source velocities"), 0);
        velocityPatternControl->addItem(tr("Fade in"), 1);
        velocityPatternControl->addItem(tr("Fade out"), 2);
        velocityPatternControl->addItem(tr("Accent every 2 steps"), 3);
        velocityPatternControl->addItem(tr("Accent every 3 steps"), 4);
        form->addRow(tr("Velocity pattern"), velocityPatternControl);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;

        if (midiEditor->arpeggiateSelectedChord(directionControl->currentData().toInt(),
                                                octaveControl->value(), gateControl->value(),
                                                velocityPatternControl->currentData().toInt()))
            statusBar()->showMessage(tr("Arpeggiated the selected chord on the current grid"), 2000);
        else
            statusBar()->showMessage(tr("Select chord notes with the same start and length"), 2400);
    });
    QAction* strumAction = editMenu->addAction(tr("Strum selected chord upward"));
    connect(strumAction, &QAction::triggered, this, [this]
    {
        if (midiEditor->strumSelectedChord())
            statusBar()->showMessage(tr("Strummed selected chord notes on the piano roll"), 1800);
        else
            statusBar()->showMessage(tr("Select chord notes with a shared start time"), 2400);
    });

    QMenu* patternMenu = menuBar()->addMenu(tr("&Track"));
    patternMenu->addAction(addTrackAction);
    patternMenu->addAction(addDrumTrackAction);
    if (addClapInstrumentTrackAction != nullptr)
        patternMenu->addAction(addClapInstrumentTrackAction);
    patternMenu->addAction(addPatternAction);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(browserDock->toggleViewAction());
    viewMenu->addAction(mixerDock->toggleViewAction());

    QMenu* transportMenu = menuBar()->addMenu(tr("&Transport"));
    transportMenu->addAction(playAction);
    transportMenu->addAction(stopAction);
    transportMenu->addAction(recordAction);
    transportMenu->addAction(audioMonitorAction);
    transportMenu->addAction(midiRecordAction);
    transportMenu->addAction(midiMonitorAction);
    transportMenu->addAction(loopAction);
    transportMenu->addAction(metronomeAction);

    QMenu* arrangementMenu = menuBar()->addMenu(tr("&Arrangement"));
    arrangementMenu->addAction(tr("Add marker at playhead"), this, [this]
    {
        addTimelineMarker(audioEngine.positionBeats());
    });

    QMenu* midiMenu = menuBar()->addMenu(tr("&MIDI"));
    midiMenu->addAction(tr("Set pitch-bend range…"), this, [this]
    {
        bool accepted = false;
        const double range = QInputDialog::getDouble(
            this, tr("MIDI pitch-bend range"),
            tr("Set the project-wide pitch-bend range in semitones:"),
            project.pitchBendRangeSemitones, 0.01, 48.0, 2, &accepted, Qt::WindowFlags(), 0.5);
        if (!accepted || std::abs(range - project.pitchBendRangeSemitones) < 1.0e-9)
            return;
        undoStack.push(new PitchBendRangeCommand(this, project.pitchBendRangeSemitones, range));
    });
    midiControllerListenAction = new QAction(tr("Enable controller mappings"), this);
    midiControllerListenAction->setCheckable(true);
    midiControllerListenAction->setEnabled(MidiInput::isSupported());
    midiControllerListenAction->setToolTip(tr("Listen for mapped MIDI CC controls on the selected input"));
    midiMenu->addAction(midiControllerListenAction);
    connect(midiControllerListenAction, &QAction::toggled,
            this, &MainWindow::setMidiControllerListening);

    const std::array<QString, 4> controllerTargets = {
        tr("Master level"), tr("Selected track level"), tr("Selected track pan"), tr("Tempo")
    };
    QMenu* learnControllerMenu = midiMenu->addMenu(tr("Learn controller for"));
    QMenu* clearControllerMenu = midiMenu->addMenu(tr("Clear controller mapping for"));
    for (int target = 0; target < static_cast<int>(MidiControllerTarget::Tempo) + 1; ++target)
    {
        QAction* learnAction = learnControllerMenu->addAction(controllerTargets[static_cast<size_t>(target)]);
        connect(learnAction, &QAction::triggered, this,
                [this, target] { beginMidiControllerLearn(target); });
        QAction* clearAction = clearControllerMenu->addAction(controllerTargets[static_cast<size_t>(target)]);
        connect(clearAction, &QAction::triggered, this,
                [this, target] { clearMidiControllerMapping(target); });
    }

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("About Awesome Studio"), this, [this]
    {
        QMessageBox::about(this, tr("About Awesome Studio"),
                           tr("Awesome Studio\nA pattern-based MIDI workstation with a Playlist, instrument tracks, and a built-in synth."));
    });
}

InstrumentTrack* MainWindow::selectedTrack()
{
    return project.findTrack(selectedTrackId);
}

MidiClip* MainWindow::selectedClip()
{
    return project.findClip(selectedTrackId, selectedClipId);
}

void MainWindow::refreshPianoRollGhostNotes()
{
    QVector<MidiNote> ghostNotes;
    const InstrumentTrack* track = project.findTrack(selectedTrackId);
    if (track != nullptr && track->instrumentType != InstrumentType::Audio)
    {
        for (const MidiClip& clip : track->clips)
        {
            if (clip.id == selectedClipId || clip.isAudioRegion())
                continue;
            ghostNotes += clip.notes;
        }
    }
    midiEditor->setGhostNotes(ghostNotes);
}

void MainWindow::refreshProjectUi()
{
    if (automationWriteSession.active)
        finishAutomationWrite();
    if (project.findTrack(selectedTrackId) == nullptr)
        selectedTrackId = project.tracks.isEmpty() ? 0 : project.tracks.first().id;
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
    {
        selectedClipId = 0;
    }
    else if (project.findClip(selectedTrackId, selectedClipId) == nullptr)
    {
        selectedClipId = track->clips.isEmpty() ? 0 : track->clips.first().id;
    }

    const QSignalBlocker trackListBlocker(trackList);
    trackList->clear();
    for (const InstrumentTrack& item : project.tracks)
    {
        auto* listItem = new QListWidgetItem(item.name, trackList);
        listItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(item.id));
        if (item.id == selectedTrackId)
            trackList->setCurrentItem(listItem);
    }

    const QSignalBlocker waveformBlocker(waveformControl);
    const QSignalBlocker secondOscillatorEnabledBlocker(secondOscillatorEnabledControl);
    const QSignalBlocker secondOscillatorWaveformBlocker(secondOscillatorWaveformControl);
    const QSignalBlocker secondOscillatorDetuneBlocker(secondOscillatorDetuneControl);
    const QSignalBlocker secondOscillatorMixBlocker(secondOscillatorMixControl);
    const QSignalBlocker synthPresetBlocker(synthPresetControl);
    const QSignalBlocker synthFilterEnabledBlocker(synthFilterEnabledControl);
    const QSignalBlocker synthFilterTypeBlocker(synthFilterTypeControl);
    const QSignalBlocker synthFilterCutoffBlocker(synthFilterCutoffControl);
    const QSignalBlocker synthFilterResonanceBlocker(synthFilterResonanceControl);
    const QSignalBlocker synthFilterEnvelopeEnabledBlocker(synthFilterEnvelopeEnabledControl);
    const QSignalBlocker synthFilterEnvelopeBlocker0(synthFilterEnvelopeControls[0]);
    const QSignalBlocker synthFilterEnvelopeBlocker1(synthFilterEnvelopeControls[1]);
    const QSignalBlocker synthFilterEnvelopeBlocker2(synthFilterEnvelopeControls[2]);
    const QSignalBlocker synthFilterEnvelopeBlocker3(synthFilterEnvelopeControls[3]);
    const QSignalBlocker synthFilterEnvelopeBlocker4(synthFilterEnvelopeControls[4]);
    const int waveformIndex = track == nullptr ? 0 :
        waveformControl->findData(static_cast<int>(track->waveform));
    waveformControl->setCurrentIndex(std::max(0, waveformIndex));
    const bool synthTrackSelected = track != nullptr && track->instrumentType == InstrumentType::Synth;
    synthPresetPanel->setVisible(synthTrackSelected);
    synthPresetControl->setEnabled(synthTrackSelected);
    int matchingPresetIndex = -1;
    if (synthTrackSelected)
    {
        for (size_t index = 0; index < factorySynthPresets.size(); ++index)
        {
            if (matchesFactorySynthPreset(*track, factorySynthPresets[index]))
            {
                matchingPresetIndex = static_cast<int>(index);
                break;
            }
        }
    }
    synthPresetControl->setCurrentIndex(matchingPresetIndex);
    waveformControl->setEnabled(synthTrackSelected);
    oscillatorPanel->setVisible(synthTrackSelected);
    const bool secondOscillatorEnabled = synthTrackSelected && track->secondOscillatorEnabled;
    secondOscillatorEnabledControl->setChecked(secondOscillatorEnabled);
    secondOscillatorEnabledControl->setEnabled(synthTrackSelected);
    const int secondWaveformIndex = track == nullptr ? 0 :
        secondOscillatorWaveformControl->findData(static_cast<int>(track->secondOscillatorWaveform));
    secondOscillatorWaveformControl->setCurrentIndex(std::max(0, secondWaveformIndex));
    secondOscillatorDetuneControl->setValue(track == nullptr ? 0.0 : track->secondOscillatorDetuneCents);
    secondOscillatorMixControl->setValue(track == nullptr ? 50.0 : track->secondOscillatorMix * 100.0);
    secondOscillatorWaveformControl->setEnabled(secondOscillatorEnabled);
    secondOscillatorDetuneControl->setEnabled(secondOscillatorEnabled);
    secondOscillatorMixControl->setEnabled(secondOscillatorEnabled);
    synthEnvelopePanel->setVisible(synthTrackSelected);
    synthFilterPanel->setVisible(synthTrackSelected);
    const bool synthFilterEnabled = synthTrackSelected && track->synthFilterEnabled;
    synthFilterEnabledControl->setChecked(synthFilterEnabled);
    synthFilterEnabledControl->setEnabled(synthTrackSelected);
    const int synthFilterTypeIndex = track == nullptr ? 0 :
        synthFilterTypeControl->findData(static_cast<int>(track->synthFilterType));
    synthFilterTypeControl->setCurrentIndex(std::max(0, synthFilterTypeIndex));
    synthFilterTypeControl->setEnabled(synthFilterEnabled);
    synthFilterCutoffControl->setValue(track == nullptr ? 20000.0 : track->synthFilterCutoffHz);
    synthFilterResonanceControl->setValue(track == nullptr ? 0.707 : track->synthFilterResonance);
    synthFilterCutoffControl->setEnabled(synthFilterEnabled);
    synthFilterResonanceControl->setEnabled(synthFilterEnabled);
    synthFilterEnvelopePanel->setVisible(synthTrackSelected);
    synthFilterEnvelopePanel->setEnabled(synthFilterEnabled);
    const bool synthFilterEnvelopeEnabled = synthTrackSelected &&
        track->synthFilterEnvelopeEnabled;
    synthFilterEnvelopeEnabledControl->setChecked(synthFilterEnvelopeEnabled);
    const std::array<float, 5> filterEnvelopeValues = track == nullptr
        ? std::array<float, 5>{24.0f, 0.008f, 0.2f, 0.0f, 0.2f}
        : std::array<float, 5>{track->synthFilterEnvelopeAmountSemitones,
            track->synthFilterEnvelopeAttackSeconds, track->synthFilterEnvelopeDecaySeconds,
            track->synthFilterEnvelopeSustainLevel, track->synthFilterEnvelopeReleaseSeconds};
    for (size_t index = 0; index < synthFilterEnvelopeControls.size(); ++index)
    {
        synthFilterEnvelopeControls[index]->setValue(filterEnvelopeValues[index]);
        synthFilterEnvelopeControls[index]->setEnabled(
            synthFilterEnabled && synthFilterEnvelopeEnabled);
    }
    const std::array<float, 4> envelopeValues = track == nullptr
        ? std::array<float, 4>{0.008f, 0.0f, 1.0f, 0.06f}
        : std::array<float, 4>{track->attackSeconds, track->decaySeconds,
                               track->sustainLevel, track->releaseSeconds};
    for (size_t index = 0; index < envelopeControls.size(); ++index)
    {
        const QSignalBlocker envelopeBlocker(envelopeControls[index]);
        envelopeControls[index]->setValue(envelopeValues[index]);
        envelopeControls[index]->setEnabled(synthTrackSelected);
    }
    const bool clapInstrumentSelected = track != nullptr &&
        track->instrumentType == InstrumentType::ClapInstrument;
    bool clapInstrumentAvailable = false;
    bool clapInstrumentHasControls = false;
    ClapPluginDescriptor clapInstrumentDescriptor;
    if (clapInstrumentSelected)
    {
        clapInstrumentAvailable = ClapAudioPluginInstance::describe(
            track->clapPluginId, ClapPluginKind::Instrument, clapInstrumentDescriptor);
        if (clapInstrumentAvailable)
        {
            clapInstrumentHasControls = std::any_of(
                clapInstrumentDescriptor.parameters.cbegin(),
                clapInstrumentDescriptor.parameters.cend(),
                [](const ClapPluginParameter& parameter)
                { return !parameter.hidden && !parameter.readOnly; });
        }
    }
    clapInstrumentControlsButton->setVisible(clapInstrumentSelected);
    clapInstrumentControlsButton->setEnabled(clapInstrumentAvailable && clapInstrumentHasControls);
    clapInstrumentControlsButton->setToolTip(
        !clapInstrumentAvailable ? tr("This CLAP instrument is not installed") :
        !clapInstrumentHasControls ? tr("This instrument exposes no editable parameters") :
        tr("Edit the selected CLAP instrument's advertised parameters"));
    equalizerControls->setVisible(track != nullptr);
    const std::array<float, 3> equalizerValues = track == nullptr
        ? std::array<float, 3>{0.0f, 0.0f, 0.0f}
        : std::array<float, 3>{track->eqLowDb, track->eqMidDb, track->eqHighDb};
    for (size_t index = 0; index < equalizerGainControls.size(); ++index)
    {
        const QSignalBlocker equalizerBlocker(equalizerGainControls[index]);
        equalizerGainControls[index]->setValue(equalizerValues[index]);
        equalizerGainControls[index]->setEnabled(track != nullptr);
    }
    const double lowCrossover = track == nullptr ? 120.0 : track->eqLowCrossoverHz;
    const double highCrossover = track == nullptr ? 6000.0 : track->eqHighCrossoverHz;
    const QSignalBlocker lowCrossoverBlocker(equalizerCrossoverControls[0]);
    const QSignalBlocker highCrossoverBlocker(equalizerCrossoverControls[1]);
    equalizerCrossoverControls[0]->setRange(20.0, std::min(2000.0, highCrossover - 1.0));
    equalizerCrossoverControls[1]->setRange(std::max(500.0, lowCrossover + 1.0), 20000.0);
    equalizerCrossoverControls[0]->setValue(lowCrossover);
    equalizerCrossoverControls[1]->setValue(highCrossover);
    equalizerCrossoverControls[0]->setEnabled(track != nullptr);
    equalizerCrossoverControls[1]->setEnabled(track != nullptr);
    refreshParametricEqualizerControls();
    refreshAutomationEditor();
    instrumentHeading->setText(track == nullptr ? tr("SELECTED INSTRUMENT") :
                               track->instrumentType == InstrumentType::DrumRack ? tr("DRUM RACK") :
                               track->instrumentType == InstrumentType::Audio ? tr("AUDIO SOURCE") :
                               clapInstrumentSelected
                                   ? tr("CLAP INSTRUMENT · %1%2")
                                         .arg(track->clapPluginName.isEmpty()
                                                  ? track->name : track->clapPluginName,
                                              clapInstrumentAvailable ? QString() : tr(" (unavailable)"))
                                   : tr("SELECTED INSTRUMENT"));

    const MidiClip* clip = selectedClip();
    const QSignalBlocker audioFadeInBlocker(audioFadeInControl);
    const QSignalBlocker audioFadeOutBlocker(audioFadeOutControl);
    const QSignalBlocker audioFadeInCurveBlocker(audioFadeInCurveControl);
    const QSignalBlocker audioFadeOutCurveBlocker(audioFadeOutCurveControl);
    const QSignalBlocker audioFadeInShapeBlocker(audioFadeInShapeControl);
    const QSignalBlocker audioFadeOutShapeBlocker(audioFadeOutShapeControl);
    const bool audioRegionSelected = clip != nullptr && clip->isAudioRegion();
    audioRegionControls->setVisible(audioRegionSelected);
    audioFadeInControl->setMaximum(audioRegionSelected ? clip->lengthBeats : 0.0);
    audioFadeOutControl->setMaximum(audioRegionSelected ? clip->lengthBeats : 0.0);
    audioFadeInControl->setValue(audioRegionSelected ? clip->audioFadeInBeats : 0.0);
    audioFadeOutControl->setValue(audioRegionSelected ? clip->audioFadeOutBeats : 0.0);
    audioFadeInCurveControl->setCurrentIndex(audioRegionSelected
        ? audioFadeInCurveControl->findData(static_cast<int>(clip->audioFadeInCurve)) : 0);
    audioFadeOutCurveControl->setCurrentIndex(audioRegionSelected
        ? audioFadeOutCurveControl->findData(static_cast<int>(clip->audioFadeOutCurve)) : 0);
    audioFadeInShapeControl->setValue(audioRegionSelected ? clip->audioFadeInShape : 1.0);
    audioFadeOutShapeControl->setValue(audioRegionSelected ? clip->audioFadeOutShape : 1.0);
    audioFadeInControl->setEnabled(audioRegionSelected);
    audioFadeOutControl->setEnabled(audioRegionSelected);
    audioFadeInCurveControl->setEnabled(audioRegionSelected);
    audioFadeOutCurveControl->setEnabled(audioRegionSelected);
    audioFadeInShapeControl->setEnabled(audioRegionSelected &&
        clip->audioFadeInCurve == AudioFadeCurve::Custom);
    audioFadeOutShapeControl->setEnabled(audioRegionSelected &&
        clip->audioFadeOutCurve == AudioFadeCurve::Custom);
    audioCrossfadeButton->setEnabled(audioRegionSelected && track != nullptr &&
        !overlappingAudioRegions(project, *track, *clip).isEmpty());
    const bool drumRackSelected = track != nullptr && track->instrumentType == InstrumentType::DrumRack;
    drumSampleControls->setVisible(drumRackSelected);
    const std::array<QString, 4> drumPadNames = {
        tr("Kick"), tr("Snare"), tr("Closed hat"), tr("Open hat")
    };
    for (size_t pad = 0; pad < drumSampleButtons.size(); ++pad)
    {
        const DrumPadSample* sample = track == nullptr ? nullptr : &track->drumSamples[pad];
        const QString padName = drumPadNames[pad];
        drumSampleButtons[pad]->setText(sample == nullptr || sample->path.isEmpty()
            ? tr("Load %1 sample…").arg(padName)
            : sample->source != nullptr ? QFileInfo(sample->path).fileName()
                                        : tr("%1 · OFFLINE").arg(padName));
        drumSampleButtons[pad]->setEnabled(drumRackSelected);
        clearDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr &&
                                                !sample->path.isEmpty());
        veryLowVelocityDrumSampleButtons[pad]->setText(
            sample == nullptr || sample->veryLowVelocityPath.isEmpty()
                ? tr("Load %1 extra-low layer…").arg(padName)
                : sample->veryLowVelocitySource != nullptr
                    ? QFileInfo(sample->veryLowVelocityPath).fileName()
                    : tr("%1 extra-low · OFFLINE").arg(padName));
        veryLowVelocityDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr);
        clearVeryLowVelocityDrumSampleButtons[pad]->setEnabled(
            drumRackSelected && sample != nullptr && !sample->veryLowVelocityPath.isEmpty());
        lowVelocityDrumSampleButtons[pad]->setText(sample == nullptr || sample->lowVelocityPath.isEmpty()
            ? tr("Load %1 low-velocity layer…").arg(padName)
            : sample->lowVelocitySource != nullptr ? QFileInfo(sample->lowVelocityPath).fileName()
                                                   : tr("%1 low layer · OFFLINE").arg(padName));
        lowVelocityDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr);
        clearLowVelocityDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr &&
                                                           !sample->lowVelocityPath.isEmpty());
        highVelocityDrumSampleButtons[pad]->setText(sample == nullptr || sample->highVelocityPath.isEmpty()
            ? tr("Load %1 high-velocity layer…").arg(padName)
            : sample->highVelocitySource != nullptr ? QFileInfo(sample->highVelocityPath).fileName()
                                                    : tr("%1 layer · OFFLINE").arg(padName));
        highVelocityDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr);
        clearHighVelocityDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr &&
                                                            !sample->highVelocityPath.isEmpty());
        veryHighVelocityDrumSampleButtons[pad]->setText(
            sample == nullptr || sample->veryHighVelocityPath.isEmpty()
                ? tr("Load %1 extra-high layer…").arg(padName)
                : sample->veryHighVelocitySource != nullptr
                    ? QFileInfo(sample->veryHighVelocityPath).fileName()
                    : tr("%1 extra-high · OFFLINE").arg(padName));
        veryHighVelocityDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr);
        clearVeryHighVelocityDrumSampleButtons[pad]->setEnabled(
            drumRackSelected && sample != nullptr && !sample->veryHighVelocityPath.isEmpty());
        const QSignalBlocker tuneBlocker(drumSampleTuneControls[pad]);
        const QSignalBlocker startBlocker(drumSampleStartControls[pad]);
        const QSignalBlocker loopBlocker(drumSampleLoopControls[pad]);
        const QSignalBlocker attackBlocker(drumPadAttackControls[pad]);
        const QSignalBlocker decayBlocker(drumPadDecayControls[pad]);
        const QSignalBlocker sustainBlocker(drumPadSustainControls[pad]);
        const QSignalBlocker releaseBlocker(drumPadReleaseControls[pad]);
        const QSignalBlocker veryLowThresholdBlocker(veryLowVelocityThresholdControls[pad]);
        const QSignalBlocker lowThresholdBlocker(lowVelocityThresholdControls[pad]);
        const QSignalBlocker thresholdBlocker(highVelocityThresholdControls[pad]);
        const QSignalBlocker veryHighThresholdBlocker(veryHighVelocityThresholdControls[pad]);
        const bool sampleAssigned = drumRackSelected && sample != nullptr &&
            sample->hasSampleAssignment();
        const int selectedLayer = drumLayerSettingsControls[pad]->currentData().toInt();
        const bool layerSpecific = selectedLayer >= 0 && selectedLayer < 5;
        const bool selectedLayerAssigned = layerSpecific && sample != nullptr &&
            sample->hasLayerAssignment(selectedLayer);
        const DrumLayerSettings displayedSettings = sample == nullptr
            ? DrumLayerSettings{}
            : layerSpecific ? sample->layerSettings(selectedLayer) : sample->sharedLayerSettings();
        drumSampleTuneControls[pad]->setValue(displayedSettings.tuningSemitones);
        drumSampleStartControls[pad]->setValue(displayedSettings.startPositionPercent);
        drumSampleLoopControls[pad]->setChecked(displayedSettings.loopEnabled);
        drumPadAttackControls[pad]->setValue(displayedSettings.attackSeconds);
        drumPadDecayControls[pad]->setValue(displayedSettings.decaySeconds);
        drumPadSustainControls[pad]->setValue(displayedSettings.sustainLevel);
        drumPadReleaseControls[pad]->setValue(displayedSettings.releaseSeconds);
        veryLowVelocityThresholdControls[pad]->setRange(1,
            sample == nullptr ? 124 : sample->lowVelocityThreshold);
        lowVelocityThresholdControls[pad]->setValue(sample == nullptr ? 48 : sample->lowVelocityThreshold);
        lowVelocityThresholdControls[pad]->setRange(
            sample == nullptr ? 2 : sample->veryLowVelocityThreshold,
            sample == nullptr ? 125 : sample->highVelocityThreshold - 1);
        highVelocityThresholdControls[pad]->setRange(
            sample == nullptr ? 3 : sample->lowVelocityThreshold + 1,
            sample == nullptr ? 126 : sample->veryHighVelocityThreshold);
        veryHighVelocityThresholdControls[pad]->setRange(
            sample == nullptr ? 4 : sample->highVelocityThreshold, 127);
        veryLowVelocityThresholdControls[pad]->setValue(
            sample == nullptr ? 24 : sample->veryLowVelocityThreshold);
        lowVelocityThresholdControls[pad]->setValue(
            sample == nullptr ? 48 : sample->lowVelocityThreshold);
        highVelocityThresholdControls[pad]->setValue(
            sample == nullptr ? 100 : sample->highVelocityThreshold);
        veryHighVelocityThresholdControls[pad]->setValue(
            sample == nullptr ? 120 : sample->veryHighVelocityThreshold);
        drumLayerSettingsControls[pad]->setEnabled(drumRackSelected);
        resetDrumLayerSettingsButtons[pad]->setVisible(layerSpecific);
        resetDrumLayerSettingsButtons[pad]->setEnabled(layerSpecific && sample != nullptr &&
            sample->velocityLayerSettingsCustom[static_cast<size_t>(selectedLayer)]);
        const bool playbackSettingsEnabled = layerSpecific
            ? selectedLayerAssigned : sampleAssigned;
        drumSampleTuneControls[pad]->setEnabled(playbackSettingsEnabled);
        drumSampleStartControls[pad]->setEnabled(playbackSettingsEnabled);
        drumSampleLoopControls[pad]->setEnabled(playbackSettingsEnabled);
        const bool envelopeSettingsEnabled = layerSpecific
            ? selectedLayerAssigned : drumRackSelected;
        drumPadAttackControls[pad]->setEnabled(envelopeSettingsEnabled);
        drumPadDecayControls[pad]->setEnabled(envelopeSettingsEnabled);
        drumPadSustainControls[pad]->setEnabled(envelopeSettingsEnabled);
        drumPadReleaseControls[pad]->setEnabled(envelopeSettingsEnabled);
        veryLowVelocityThresholdControls[pad]->setEnabled(sampleAssigned);
        lowVelocityThresholdControls[pad]->setEnabled(sampleAssigned);
        highVelocityThresholdControls[pad]->setEnabled(sampleAssigned);
        veryHighVelocityThresholdControls[pad]->setEnabled(sampleAssigned);
    }
    midiEditor->setTimeSignature(project.beatsPerBar, project.beatUnit);
    midiEditor->setPatternLengthBeats(clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats);
    midiEditor->setNotes(clip == nullptr ? QVector<MidiNote>{} : clip->notes);
    midiEditor->setPitchBends(clip == nullptr ? QVector<MidiPitchBend>{} : clip->pitchBends);
    midiEditor->setPitchBendLaneEnabled(clip != nullptr && !clip->isAudioRegion());
    midiEditor->setPitchBendRange(project.pitchBendRangeSemitones);
    midiEditor->setDefaultPitchBendChannel(midiExportChannelForTrack(project, selectedTrackId));
    refreshPianoRollGhostNotes();
    const bool drumTrack = track != nullptr && track->instrumentType == InstrumentType::DrumRack;
    editorTabs->setTabEnabled(editorTabs->indexOf(stepSequencer), drumTrack);
    stepSequencer->setEnabled(drumTrack);
    stepSequencer->setPattern(clip == nullptr ? QVector<MidiNote>{} : clip->notes,
                              clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats,
                              project.quarterNotesPerBar(), project.nextNoteId(), clip != nullptr,
                              clip == nullptr ? 16 : clip->stepsPerBar);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);

    const QSignalBlocker tempoBlocker(tempoControl);
    const QSignalBlocker meterBlocker(timeSignatureControl);
    const QSignalBlocker countInBlocker(countInControl);
    const QSignalBlocker clipLengthBlocker(clipLengthControl);
    const QSignalBlocker loopBlocker(loopAction);
    const QSignalBlocker metronomeBlocker(metronomeAction);
    tempoControl->setValue(project.tempoBpm);
    const QVariantList currentMeter{project.beatsPerBar, project.beatUnit};
    int meterIndex = timeSignatureControl->findData(currentMeter);
    if (meterIndex < 0)
    {
        timeSignatureControl->addItem(QStringLiteral("%1/%2").arg(project.beatsPerBar).arg(project.beatUnit),
                                      currentMeter);
        meterIndex = timeSignatureControl->count() - 1;
    }
    timeSignatureControl->setCurrentIndex(meterIndex);
    const int countInIndex = countInControl->findData(project.countInBars);
    countInControl->setCurrentIndex(countInIndex < 0 ? 0 : countInIndex);
    metronomeAction->setChecked(project.metronomeEnabled);
    clipLengthControl->setEnabled(clip != nullptr);
    clipLengthControl->setValue(clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats);
    loopAction->setChecked(project.loopEnabled);
    refreshMixer();
    audioEngine.setProject(project);
    audioEngine.setMasterGain(project.masterGain);
}

void MainWindow::refreshMixer()
{
    if (mixerScrollArea == nullptr)
        return;

    mixerTrackMeters.clear();
    mixerBusMeters.clear();
    masterPeakMeter = nullptr;
    auto* content = new QWidget(mixerScrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    const auto createPeakMeter = [](QWidget* parent)
    {
        auto* meter = new QProgressBar(parent);
        meter->setRange(0, 1000);
        meter->setValue(0);
        meter->setTextVisible(false);
        meter->setFixedHeight(9);
        meter->setStyleSheet(QStringLiteral(
            "QProgressBar { background: #20262e; border: 1px solid #3a434e; border-radius: 3px; }"
            "QProgressBar::chunk { border-radius: 2px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "stop:0 #39c98c, stop:0.72 #c9c45b, stop:0.9 #ed8c4a, stop:1 #e75c5c); }"));
        return meter;
    };
    const auto effectCount = [this]
    {
        qsizetype count = project.masterEffects.size();
        for (const MixerBus& bus : project.buses)
            count += bus.effects.size();
        for (const InstrumentTrack& track : project.tracks)
            count += track.effects.size();
        return count;
    };
    const auto createEffectControls = [this](QFormLayout* form, QWidget* parent,
        const QVector<MixerEffect>& effects,
        const std::function<void(qsizetype, const std::function<void(MixerEffect&)>&)>& updateEffect,
        const std::function<void(MixerEffectType)>& appendEffect,
        const std::function<void(qsizetype)>& removeEffect,
        const std::function<void(qsizetype, qsizetype)>& moveEffect,
        bool canAddEffect, quint64 automationTrackId)
    {
        auto* chain = new QGroupBox(tr("Effect inserts"), parent);
        auto* chainLayout = new QVBoxLayout(chain);
        chainLayout->setContentsMargins(6, 6, 6, 6);
        const auto addFloatControl = [this](QFormLayout* effectForm, const QString& label,
            float value, float minimum, float maximum, float step, int decimals,
            const QString& suffix, qsizetype effectIndex,
            const std::function<void(qsizetype, const std::function<void(MixerEffect&)>&)>& update,
            const std::function<void(MixerEffect&, float)>& assign)
        {
            auto* control = new QDoubleSpinBox(effectForm->parentWidget());
            control->setRange(minimum, maximum);
            control->setSingleStep(step);
            control->setDecimals(decimals);
            control->setSuffix(suffix);
            control->setValue(value);
            effectForm->addRow(label, control);
            connect(control, &QDoubleSpinBox::editingFinished, this,
                    [control, effectIndex, update, assign]
            {
                const float value = static_cast<float>(control->value());
                update(effectIndex, [assign, value](MixerEffect& effect) { assign(effect, value); });
            });
            return control;
        };
        for (qsizetype index = 0; index < effects.size(); ++index)
        {
            const MixerEffect effect = effects[index];
            const QString effectName = effect.type == MixerEffectType::Delay ? tr("Delay") :
                effect.type == MixerEffectType::Reverb ? tr("Reverb") :
                effect.type == MixerEffectType::Compressor ? tr("Compressor") :
                effect.type == MixerEffectType::LowPass ? tr("Low-pass filter") :
                effect.pluginName.isEmpty()
                    ? effect.type == MixerEffectType::ClapPlugin ? tr("CLAP plug-in") : tr("LV2 plug-in")
                    : effect.pluginName;
            auto* effectBox = new QGroupBox(tr("%1. %2").arg(index + 1).arg(effectName), chain);
            auto* effectForm = new QFormLayout(effectBox);
            auto* stateRow = new QWidget(effectBox);
            auto* stateLayout = new QHBoxLayout(stateRow);
            stateLayout->setContentsMargins(0, 0, 0, 0);
            auto* enabled = new QCheckBox(tr("Enabled"), stateRow);
            enabled->setChecked(effect.enabled);
            auto* moveUp = new QPushButton(QStringLiteral("↑"), stateRow);
            moveUp->setEnabled(index > 0);
            moveUp->setToolTip(tr("Move this insert earlier in the chain"));
            auto* moveDown = new QPushButton(QStringLiteral("↓"), stateRow);
            moveDown->setEnabled(index + 1 < effects.size());
            moveDown->setToolTip(tr("Move this insert later in the chain"));
            auto* remove = new QPushButton(tr("Remove"), stateRow);
            stateLayout->addWidget(enabled);
            stateLayout->addStretch(1);
            stateLayout->addWidget(moveUp);
            stateLayout->addWidget(moveDown);
            stateLayout->addWidget(remove);
            effectForm->addRow(stateRow);
            connect(enabled, &QCheckBox::toggled, this,
                    [index, updateEffect](bool checked)
            {
                updateEffect(index, [checked](MixerEffect& effect) { effect.enabled = checked; });
            });
            connect(remove, &QPushButton::clicked, this, [removeEffect, index]
            {
                removeEffect(index);
            });
            connect(moveUp, &QPushButton::clicked, this, [moveEffect, index]
            {
                moveEffect(index, index - 1);
            });
            connect(moveDown, &QPushButton::clicked, this, [moveEffect, index]
            {
                moveEffect(index, index + 1);
            });
            addFloatControl(effectForm, tr("Wet"), effect.wet * 100.0f, 0.0f, 100.0f,
                            1.0f, 0, QStringLiteral("%"), index, updateEffect,
                            [](MixerEffect& target, float value) { target.wet = value / 100.0f; });
            if (effect.type == MixerEffectType::Delay)
            {
                addFloatControl(effectForm, tr("Time"), effect.delayMs, 1.0f, 2000.0f,
                                1.0f, 0, tr(" ms"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.delayMs = value; });
                addFloatControl(effectForm, tr("Feedback"), effect.feedback * 100.0f, 0.0f, 95.0f,
                                1.0f, 0, QStringLiteral("%"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.feedback = value / 100.0f; });
            }
            else if (effect.type == MixerEffectType::Reverb)
            {
                addFloatControl(effectForm, tr("Room size"), effect.roomSize * 100.0f, 0.0f, 100.0f,
                                1.0f, 0, QStringLiteral("%"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.roomSize = value / 100.0f; });
                addFloatControl(effectForm, tr("Damping"), effect.damping * 100.0f, 0.0f, 100.0f,
                                1.0f, 0, QStringLiteral("%"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.damping = value / 100.0f; });
            }
            else if (effect.type == MixerEffectType::Compressor)
            {
                addFloatControl(effectForm, tr("Threshold"), effect.thresholdDb, -60.0f, 0.0f,
                                0.5f, 1, tr(" dB"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.thresholdDb = value; });
                addFloatControl(effectForm, tr("Ratio"), effect.ratio, 1.0f, 20.0f,
                                0.1f, 1, QString(), index, updateEffect,
                                [](MixerEffect& target, float value) { target.ratio = value; });
                addFloatControl(effectForm, tr("Attack"), effect.attackMs, 0.1f, 200.0f,
                                0.1f, 1, tr(" ms"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.attackMs = value; });
                addFloatControl(effectForm, tr("Release"), effect.releaseMs, 1.0f, 2000.0f,
                                1.0f, 0, tr(" ms"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.releaseMs = value; });
                addFloatControl(effectForm, tr("Makeup"), effect.makeupDb, -12.0f, 24.0f,
                                0.5f, 1, tr(" dB"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.makeupDb = value; });
            }
            else if (isPluginEffect(effect.type))
            {
                auto* pluginUri = new QLabel(effect.pluginUri, effectBox);
                pluginUri->setWordWrap(true);
                pluginUri->setTextInteractionFlags(Qt::TextSelectableByMouse);
                effectForm->addRow(effect.type == MixerEffectType::Lv2Plugin
                                       ? tr("LV2 URI") : tr("CLAP ID"), pluginUri);
                QVector<Lv2EffectParameter> parameters;
                bool pluginAvailable = false;
                if (effect.type == MixerEffectType::Lv2Plugin)
                {
                    Lv2EffectDescriptor descriptor;
                    if (Lv2AudioEffectInstance::describe(effect.pluginUri, descriptor))
                    {
                        parameters = descriptor.parameters;
                        pluginAvailable = true;
                    }
                }
                else if (effect.type == MixerEffectType::ClapPlugin)
                {
                    ClapPluginDescriptor descriptor;
                    if (ClapAudioPluginInstance::describe(
                            effect.pluginUri, ClapPluginKind::AudioEffect, descriptor))
                    {
                        pluginAvailable = true;
                        for (const ClapPluginParameter& clapParameter : descriptor.parameters)
                        {
                            Lv2EffectParameter parameter;
                            parameter.symbol = QString::number(clapParameter.id);
                            parameter.name = clapParameter.name;
                            parameter.minimum = clapParameter.minimum;
                            parameter.maximum = clapParameter.maximum;
                            parameter.defaultValue = clapParameter.defaultValue;
                            parameter.integer = clapParameter.integer;
                            parameter.toggled = clapParameter.toggled;
                            parameter.hidden = clapParameter.hidden;
                            parameter.readOnly = clapParameter.readOnly;
                            parameters.push_back(std::move(parameter));
                        }
                    }
                }
                if (pluginAvailable)
                {
                    for (qsizetype parameterIndex = 0;
                         parameterIndex < parameters.size(); ++parameterIndex)
                    {
                        const Lv2EffectParameter parameter = parameters[parameterIndex];
                        if (parameter.hidden || parameter.readOnly)
                            continue;
                        const float value = parameterIndex < effect.pluginParameterValues.size()
                            ? effect.pluginParameterValues[parameterIndex] : parameter.defaultValue;
                        const float step = parameter.toggled || parameter.integer ? 1.0f :
                            std::max(0.001f, (parameter.maximum - parameter.minimum) / 1000.0f);
                        const int decimals = parameter.toggled || parameter.integer ? 0 :
                            parameter.maximum - parameter.minimum > 100.0f ? 2 : 3;
                        QDoubleSpinBox* parameterControl = addFloatControl(effectForm, parameter.name,
                            std::clamp(value, parameter.minimum, parameter.maximum),
                            parameter.minimum, parameter.maximum, step, decimals, QString(),
                            index, updateEffect,
                            [parameterIndex, defaultValue = parameter.defaultValue]
                            (MixerEffect& target, float newValue)
                        {
                            while (target.pluginParameterValues.size() <= parameterIndex)
                                target.pluginParameterValues.push_back(defaultValue);
                            target.pluginParameterValues[parameterIndex] = newValue;
                        });
                        if (automationTrackId != 0)
                        {
                            const int automationTarget = pluginAutomationTarget(index, parameterIndex);
                            const QString targetUri = effect.pluginUri;
                            connect(parameterControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                                    [this, automationTrackId, index, parameterIndex, automationTarget,
                                     targetUri, parameter](double newValue)
                            {
                                const float recordedValue = static_cast<float>(newValue);
                                if (automationWriteSession.active &&
                                    automationWriteSession.trackId == automationTrackId &&
                                    automationWriteSession.parameter == automationTarget &&
                                    automationWriteSession.pluginUri == targetUri)
                                {
                                    recordAutomationValue(automationTrackId, automationTarget, recordedValue);
                                    if (automationWriteModeControl->currentData().toInt() != 1)
                                        automationWriteIdleTimer->start(250);
                                    return;
                                }
                                if (automationWriteAction == nullptr || !automationWriteAction->isChecked() ||
                                    !audioEngine.isPlaying() || automationLaneParameterControl == nullptr ||
                                    automationLaneParameterControl->currentData().toInt() != automationTarget)
                                    return;
                                InstrumentTrack* track = project.findTrack(automationTrackId);
                                if (track == nullptr || index < 0 || index >= track->effects.size())
                                    return;
                                const MixerEffect& currentEffect = track->effects[index];
                                if (!isPluginEffect(currentEffect.type) ||
                                    currentEffect.pluginUri != targetUri)
                                    return;
                                const float baseValue = parameterIndex < currentEffect.pluginParameterValues.size()
                                    ? currentEffect.pluginParameterValues[parameterIndex]
                                    : parameter.defaultValue;
                                beginAutomationWrite(automationTrackId, automationTarget, baseValue,
                                    targetUri, index, parameterIndex, parameter.minimum, parameter.maximum,
                                    parameter.name);
                                if (automationWriteSession.active &&
                                    automationWriteSession.trackId == automationTrackId &&
                                    automationWriteSession.parameter == automationTarget &&
                                    automationWriteSession.pluginUri == targetUri)
                                {
                                    recordAutomationValue(automationTrackId, automationTarget, recordedValue);
                                    if (automationWriteModeControl->currentData().toInt() != 1)
                                        automationWriteIdleTimer->start(250);
                                }
                            });
                        }
                        QVector<AutomationPoint> curve =
                            parameterIndex < effect.pluginParameterAutomation.size()
                                ? effect.pluginParameterAutomation[parameterIndex]
                                : QVector<AutomationPoint>{};
                        auto* automation = new QPushButton(
                            tr("Automation (%1 keyframes)").arg(curve.size()), effectBox);
                        effectForm->addRow(QString(), automation);
                        connect(automation, &QPushButton::clicked, this,
                                [this, automation, index, parameterIndex, parameter,
                                 updateEffect, curve = std::move(curve)]() mutable
                        {
                            QVector<AutomationPoint> edited = curve;
                            if (!editPluginParameterAutomation(this,
                                    tr("%1 automation").arg(parameter.name),
                                    parameter.minimum, parameter.maximum, edited))
                                return;
                            updateEffect(index, [parameterIndex, edited](MixerEffect& target)
                            {
                                while (target.pluginParameterAutomation.size() <= parameterIndex)
                                    target.pluginParameterAutomation.push_back({});
                                target.pluginParameterAutomation[parameterIndex] = edited;
                            });
                            curve = std::move(edited);
                            automation->setText(tr("Automation (%1 keyframes)").arg(curve.size()));
                        });
                    }
                }
                else
                {
                    auto* missing = new QLabel(tr("Plug-in unavailable; its saved settings are retained."),
                                               effectBox);
                    missing->setWordWrap(true);
                    effectForm->addRow(missing);
                }
            }
            else
            {
                addFloatControl(effectForm, tr("Cutoff"), effect.cutoffHz, 20.0f, 20000.0f,
                                10.0f, 0, tr(" Hz"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.cutoffHz = value; });
                addFloatControl(effectForm, tr("Resonance"), effect.resonance, 0.1f, 10.0f,
                                0.1f, 1, QStringLiteral(" Q"), index, updateEffect,
                                [](MixerEffect& target, float value) { target.resonance = value; });
            }
            chainLayout->addWidget(effectBox);
        }
        auto* addRow = new QWidget(chain);
        auto* addLayout = new QHBoxLayout(addRow);
        addLayout->setContentsMargins(0, 0, 0, 0);
        auto* type = new QComboBox(addRow);
        type->addItem(tr("Delay"), static_cast<int>(MixerEffectType::Delay));
        type->addItem(tr("Reverb"), static_cast<int>(MixerEffectType::Reverb));
        type->addItem(tr("Compressor"), static_cast<int>(MixerEffectType::Compressor));
        type->addItem(tr("Low-pass filter"), static_cast<int>(MixerEffectType::LowPass));
        type->addItem(tr("LV2 plug-in…"), static_cast<int>(MixerEffectType::Lv2Plugin));
#if defined(AWESOME_STUDIO_HAS_CLAP)
        type->addItem(tr("CLAP plug-in…"), static_cast<int>(MixerEffectType::ClapPlugin));
#endif
        auto* add = new QPushButton(tr("Add effect"), addRow);
        add->setEnabled(canAddEffect && effects.size() < 8);
        addLayout->addWidget(type, 1);
        addLayout->addWidget(add);
        chainLayout->addWidget(addRow);
        connect(add, &QPushButton::clicked, this, [type, appendEffect]
        {
            appendEffect(static_cast<MixerEffectType>(type->currentData().toInt()));
        });
        form->addRow(chain);
    };
    const auto commitEffectChange = [this]
    {
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
        refreshAutomationEditor();
    };
    const auto finishTrackPluginAutomation = [this](quint64 trackId)
    {
        if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
            !automationWriteSession.pluginUri.isEmpty())
            finishAutomationWrite();
    };

    auto* masterBox = new QGroupBox(tr("Master"), content);
    auto* masterForm = new QFormLayout(masterBox);
    masterControl = new QSlider(Qt::Horizontal, masterBox);
    masterControl->setRange(0, 100);
    masterControl->setValue(static_cast<int>(std::round(project.masterGain * 100.0f)));
    masterGainReadout = new QLabel(tr("%1%").arg(masterControl->value()), masterBox);
    auto* masterRow = new QWidget(masterBox);
    auto* masterRowLayout = new QHBoxLayout(masterRow);
    masterRowLayout->setContentsMargins(0, 0, 0, 0);
    masterRowLayout->addWidget(masterControl, 1);
    masterRowLayout->addWidget(masterGainReadout);
    masterForm->addRow(tr("Level"), masterRow);
    masterPeakMeter = createPeakMeter(masterBox);
    masterPeakMeter->setObjectName(QStringLiteral("masterPeakMeter"));
    masterPeakMeter->setToolTip(tr("Master output peak level"));
    masterForm->addRow(tr("Peak"), masterPeakMeter);
    createEffectControls(masterForm, masterBox, project.masterEffects,
        [this, commitEffectChange](qsizetype index,
                                   const std::function<void(MixerEffect&)>& editEffect)
        {
            if (index < 0 || index >= project.masterEffects.size()) return;
            editEffect(project.masterEffects[index]);
            commitEffectChange();
        },
        [this, commitEffectChange](MixerEffectType type)
        {
            QTimer::singleShot(0, this, [this, commitEffectChange, type]
            {
                if (project.masterEffects.size() >= 8 || [&]
                    {
                        qsizetype count = project.masterEffects.size();
                        for (const MixerBus& bus : project.buses) count += bus.effects.size();
                        for (const InstrumentTrack& track : project.tracks) count += track.effects.size();
                        return count >= 48;
                    }()) return;
                MixerEffect effect;
                QString pluginError;
                if (!chooseMixerEffect(this, type, effect, pluginError))
                {
                    if (!pluginError.isEmpty())
                        statusBar()->showMessage(pluginError, 5000);
                    return;
                }
                project.masterEffects.push_back(std::move(effect));
                commitEffectChange();
                refreshMixer();
            });
        },
        [this, commitEffectChange](qsizetype index)
        {
            QTimer::singleShot(0, this, [this, commitEffectChange, index]
            {
                if (index < 0 || index >= project.masterEffects.size()) return;
                project.masterEffects.removeAt(index);
                commitEffectChange();
                refreshMixer();
            });
        },
        [this, commitEffectChange](qsizetype from, qsizetype to)
        {
            QTimer::singleShot(0, this, [this, commitEffectChange, from, to]
            {
                if (from < 0 || from >= project.masterEffects.size() ||
                    to < 0 || to >= project.masterEffects.size()) return;
                project.masterEffects.move(from, to);
                commitEffectChange();
                refreshMixer();
            });
        }, effectCount() < 48, 0);
    auto* addBusButton = new QPushButton(tr("Add mixer bus"), masterBox);
    addBusButton->setEnabled(project.buses.size() < 64);
    masterForm->addRow(addBusButton);
    layout->addWidget(masterBox);
    connect(addBusButton, &QPushButton::clicked, this, &MainWindow::addMixerBus);
    connect(masterControl, &QSlider::valueChanged, this, [this](int value)
    {
        project.masterGain = value / 100.0f;
        masterGainReadout->setText(tr("%1%").arg(value));
        audioEngine.setMasterGain(project.masterGain);
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    for (qsizetype busIndex = 0; busIndex < project.buses.size(); ++busIndex)
    {
        const MixerBus& item = project.buses[busIndex];
        const quint64 busId = item.id;
        auto* group = new QGroupBox(item.name, content);
        auto* form = new QFormLayout(group);
        auto* busName = new QLineEdit(item.name, group);
        form->addRow(tr("Name"), busName);
        auto* busOutput = new QComboBox(group);
        busOutput->addItem(tr("Master"), QVariant::fromValue<qulonglong>(0));
        for (const MixerBus& candidate : project.buses)
        {
            if (candidate.id != busId)
                busOutput->addItem(candidate.name, QVariant::fromValue<qulonglong>(candidate.id));
        }
        busOutput->setCurrentIndex(std::max(0, busOutput->findData(
            QVariant::fromValue<qulonglong>(item.outputBusId))));
        form->addRow(tr("Output"), busOutput);
        connect(busOutput, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, busId, busOutput](int index)
        {
            if (index >= 0)
                setMixerBusOutput(busId, busOutput->currentData().toULongLong());
        });
        connect(busName, &QLineEdit::editingFinished, this, [this, busId, busName]
        {
            const QString newName = busName->text().trimmed().left(80);
            if (newName.isEmpty())
            {
                const MixerBus* bus = nullptr;
                for (const MixerBus& item : project.buses)
                    if (item.id == busId) { bus = &item; break; }
                if (bus != nullptr)
                    busName->setText(bus->name);
                return;
            }
            QTimer::singleShot(0, this, [this, busId, newName]
            {
                for (MixerBus& bus : project.buses)
                {
                    if (bus.id != busId)
                        continue;
                    bus.name = newName;
                    audioEngine.setProject(project);
                    settingsDirty = true;
                    refreshDirtyState();
                    refreshMixer();
                    return;
                }
            });
        });

        auto* stateRow = new QWidget(group);
        auto* stateLayout = new QHBoxLayout(stateRow);
        stateLayout->setContentsMargins(0, 0, 0, 0);
        auto* mute = new QCheckBox(tr("Mute"), stateRow);
        auto* solo = new QCheckBox(tr("Solo"), stateRow);
        mute->setChecked(item.muted);
        solo->setChecked(item.solo);
        stateLayout->addWidget(mute);
        stateLayout->addWidget(solo);
        form->addRow(stateRow);

        auto* volume = new QSlider(Qt::Horizontal, group);
        volume->setRange(0, 200);
        volume->setValue(static_cast<int>(std::round(item.gain * 100.0f)));
        auto* volumeLabel = new QLabel(tr("%1%").arg(volume->value()), group);
        auto* volumeRow = new QWidget(group);
        auto* volumeLayout = new QHBoxLayout(volumeRow);
        volumeLayout->setContentsMargins(0, 0, 0, 0);
        volumeLayout->addWidget(volume, 1);
        volumeLayout->addWidget(volumeLabel);
        form->addRow(tr("Level"), volumeRow);

        auto* pan = new QSlider(Qt::Horizontal, group);
        pan->setRange(-100, 100);
        pan->setValue(static_cast<int>(std::round(item.pan * 100.0f)));
        auto* panLabel = new QLabel(item.pan < -0.02f ? tr("L %1").arg(std::abs(pan->value())) :
                                    item.pan > 0.02f ? tr("R %1").arg(pan->value()) : tr("Center"), group);
        auto* panRow = new QWidget(group);
        auto* panLayout = new QHBoxLayout(panRow);
        panLayout->setContentsMargins(0, 0, 0, 0);
        panLayout->addWidget(pan, 1);
        panLayout->addWidget(panLabel);
        form->addRow(tr("Pan"), panRow);

        auto* peakMeter = createPeakMeter(group);
        peakMeter->setObjectName(QStringLiteral("mixerBusPeakMeter"));
        peakMeter->setToolTip(tr("Bus peak after bus level, pan, and inserts"));
        mixerBusMeters.push_back({static_cast<size_t>(busIndex), peakMeter});
        form->addRow(tr("Peak"), peakMeter);

        auto* sendsGroup = new QGroupBox(tr("Aux sends"), group);
        auto* sendsLayout = new QVBoxLayout(sendsGroup);
        sendsLayout->setContentsMargins(6, 6, 6, 6);
        for (qsizetype sendIndex = 0; sendIndex < item.sends.size(); ++sendIndex)
        {
            const MixerSend& send = item.sends[sendIndex];
            auto* sendRow = new QWidget(sendsGroup);
            auto* sendLayout = new QHBoxLayout(sendRow);
            sendLayout->setContentsMargins(0, 0, 0, 0);
            auto* sendBus = new QComboBox(sendRow);
            for (const MixerBus& candidate : project.buses)
            {
                if (candidate.id == busId)
                    continue;
                bool usedByOtherSend = false;
                for (qsizetype otherIndex = 0; otherIndex < item.sends.size(); ++otherIndex)
                {
                    if (otherIndex != sendIndex && item.sends[otherIndex].busId == candidate.id)
                    {
                        usedByOtherSend = true;
                        break;
                    }
                }
                if (!usedByOtherSend)
                    sendBus->addItem(candidate.name, QVariant::fromValue<qulonglong>(candidate.id));
            }
            sendBus->setCurrentIndex(std::max(0, sendBus->findData(
                QVariant::fromValue<qulonglong>(send.busId))));
            auto* sendLevel = new QSlider(Qt::Horizontal, sendRow);
            sendLevel->setRange(0, 100);
            sendLevel->setValue(static_cast<int>(std::round(send.gain * 100.0f)));
            auto* sendLevelLabel = new QLabel(tr("%1%").arg(sendLevel->value()), sendRow);
            auto* removeSend = new QPushButton(tr("×"), sendRow);
            removeSend->setToolTip(tr("Remove this auxiliary send"));
            sendLayout->addWidget(sendBus, 2);
            sendLayout->addWidget(sendLevel, 3);
            sendLayout->addWidget(sendLevelLabel);
            sendLayout->addWidget(removeSend);
            sendsLayout->addWidget(sendRow);
            connect(sendBus, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, busId, sendIndex, sendBus, sendLevel](int index)
            {
                if (index >= 0)
                    setMixerBusSend(busId, sendIndex, sendBus->currentData().toULongLong(),
                                    sendLevel->value() / 100.0f);
            });
            connect(sendLevel, &QSlider::valueChanged, this,
                    [this, busId, sendIndex, sendBus, sendLevelLabel](int value)
            {
                sendLevelLabel->setText(tr("%1%").arg(value));
                setMixerBusSend(busId, sendIndex, sendBus->currentData().toULongLong(), value / 100.0f);
            });
            connect(removeSend, &QPushButton::clicked, this, [this, busId, sendIndex]
            {
                QTimer::singleShot(0, this, [this, busId, sendIndex]
                {
                    removeMixerBusSend(busId, sendIndex);
                });
            });
        }
        auto* addSendButton = new QPushButton(tr("Add auxiliary send"), sendsGroup);
        addSendButton->setEnabled(project.buses.size() > 1 && item.sends.size() < project.buses.size() - 1);
        sendsLayout->addWidget(addSendButton);
        form->addRow(sendsGroup);
        connect(addSendButton, &QPushButton::clicked, this, [this, busId]
        {
            QTimer::singleShot(0, this, [this, busId] { addMixerBusSend(busId); });
        });

        createEffectControls(form, group, item.effects,
            [this, commitEffectChange, busId](qsizetype index,
                const std::function<void(MixerEffect&)>& editEffect)
            {
                for (MixerBus& bus : project.buses)
                {
                    if (bus.id != busId || index < 0 || index >= bus.effects.size()) continue;
                    editEffect(bus.effects[index]);
                    commitEffectChange();
                    return;
                }
            },
            [this, commitEffectChange, effectCount, busId](MixerEffectType type)
            {
                QTimer::singleShot(0, this, [this, commitEffectChange, effectCount, busId, type]
                {
                    for (MixerBus& bus : project.buses)
                    {
                        if (bus.id != busId) continue;
                        if (bus.effects.size() >= 8 || effectCount() >= 48) return;
                        MixerEffect effect;
                        QString pluginError;
                        if (!chooseMixerEffect(this, type, effect, pluginError))
                        {
                            if (!pluginError.isEmpty())
                                statusBar()->showMessage(pluginError, 5000);
                            return;
                        }
                        bus.effects.push_back(std::move(effect));
                        commitEffectChange();
                        refreshMixer();
                        return;
                    }
                });
            },
            [this, commitEffectChange, busId](qsizetype index)
            {
                QTimer::singleShot(0, this, [this, commitEffectChange, busId, index]
                {
                    for (MixerBus& bus : project.buses)
                    {
                        if (bus.id != busId || index < 0 || index >= bus.effects.size()) continue;
                        bus.effects.removeAt(index);
                        commitEffectChange();
                        refreshMixer();
                        return;
                    }
                });
            },
            [this, commitEffectChange, busId](qsizetype from, qsizetype to)
            {
                QTimer::singleShot(0, this, [this, commitEffectChange, busId, from, to]
                {
                    for (MixerBus& bus : project.buses)
                    {
                        if (bus.id != busId || from < 0 || from >= bus.effects.size() ||
                            to < 0 || to >= bus.effects.size()) continue;
                        bus.effects.move(from, to);
                        commitEffectChange();
                        refreshMixer();
                        return;
                    }
                });
            }, effectCount() < 48, 0);

        auto* removeBusButton = new QPushButton(tr("Remove bus"), group);
        form->addRow(removeBusButton);
        layout->addWidget(group);

        connect(mute, &QCheckBox::toggled, this, [this, busId](bool enabled)
        {
            for (MixerBus& bus : project.buses)
            {
                if (bus.id != busId) continue;
                bus.muted = enabled;
                audioEngine.setProject(project);
                settingsDirty = true;
                refreshDirtyState();
                return;
            }
        });
        connect(solo, &QCheckBox::toggled, this, [this, busId](bool enabled)
        {
            for (MixerBus& bus : project.buses)
            {
                if (bus.id != busId) continue;
                bus.solo = enabled;
                audioEngine.setProject(project);
                settingsDirty = true;
                refreshDirtyState();
                return;
            }
        });
        connect(volume, &QSlider::valueChanged, this, [this, busId, volumeLabel](int value)
        {
            for (MixerBus& bus : project.buses)
            {
                if (bus.id != busId) continue;
                bus.gain = value / 100.0f;
                volumeLabel->setText(tr("%1%").arg(value));
                audioEngine.setProject(project);
                settingsDirty = true;
                refreshDirtyState();
                return;
            }
        });
        connect(pan, &QSlider::valueChanged, this, [this, busId, panLabel](int value)
        {
            for (MixerBus& bus : project.buses)
            {
                if (bus.id != busId) continue;
                bus.pan = value / 100.0f;
                panLabel->setText(value < -2 ? tr("L %1").arg(-value) :
                                  value > 2 ? tr("R %1").arg(value) : tr("Center"));
                audioEngine.setProject(project);
                settingsDirty = true;
                refreshDirtyState();
                return;
            }
        });
        connect(removeBusButton, &QPushButton::clicked, this,
                [this, busId] { removeMixerBus(busId); });
    }

    for (const InstrumentTrack& item : project.tracks)
    {
        const quint64 trackId = item.id;
        auto* group = new QGroupBox(item.name, content);
        auto* form = new QFormLayout(group);
        auto* stateRow = new QWidget(group);
        auto* stateLayout = new QHBoxLayout(stateRow);
        stateLayout->setContentsMargins(0, 0, 0, 0);
        auto* mute = new QCheckBox(tr("Mute"), stateRow);
        auto* solo = new QCheckBox(tr("Solo"), stateRow);
        mute->setChecked(item.muted);
        solo->setChecked(item.solo);
        stateLayout->addWidget(mute);
        stateLayout->addWidget(solo);
        form->addRow(stateRow);

        auto* outputBus = new QComboBox(group);
        outputBus->addItem(tr("Master"), QVariant::fromValue<qulonglong>(0));
        for (const MixerBus& bus : project.buses)
            outputBus->addItem(bus.name, QVariant::fromValue<qulonglong>(bus.id));
        outputBus->setCurrentIndex(std::max(0, outputBus->findData(
            QVariant::fromValue<qulonglong>(item.outputBusId))));
        form->addRow(tr("Output"), outputBus);
        connect(outputBus, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, trackId, outputBus](int index)
        {
            if (index >= 0)
                setTrackOutputBus(trackId, outputBus->currentData().toULongLong());
        });

        auto* sendsGroup = new QGroupBox(tr("Aux sends"), group);
        auto* sendsLayout = new QVBoxLayout(sendsGroup);
        sendsLayout->setContentsMargins(6, 6, 6, 6);
        for (qsizetype sendIndex = 0; sendIndex < item.sends.size(); ++sendIndex)
        {
            const MixerSend& send = item.sends[sendIndex];
            auto* sendRow = new QWidget(sendsGroup);
            auto* sendLayout = new QHBoxLayout(sendRow);
            sendLayout->setContentsMargins(0, 0, 0, 0);
            auto* sendBus = new QComboBox(sendRow);
            for (const MixerBus& bus : project.buses)
            {
                bool usedByOtherSend = false;
                for (qsizetype otherIndex = 0; otherIndex < item.sends.size(); ++otherIndex)
                {
                    if (otherIndex != sendIndex && item.sends[otherIndex].busId == bus.id)
                    {
                        usedByOtherSend = true;
                        break;
                    }
                }
                if (!usedByOtherSend)
                    sendBus->addItem(bus.name, QVariant::fromValue<qulonglong>(bus.id));
            }
            sendBus->setCurrentIndex(std::max(0, sendBus->findData(
                QVariant::fromValue<qulonglong>(send.busId))));
            auto* sendLevel = new QSlider(Qt::Horizontal, sendRow);
            sendLevel->setRange(0, 100);
            sendLevel->setValue(static_cast<int>(std::round(send.gain * 100.0f)));
            auto* sendLevelLabel = new QLabel(tr("%1%").arg(sendLevel->value()), sendRow);
            auto* removeSend = new QPushButton(tr("×"), sendRow);
            removeSend->setToolTip(tr("Remove this auxiliary send"));
            sendLayout->addWidget(sendBus, 2);
            sendLayout->addWidget(sendLevel, 3);
            sendLayout->addWidget(sendLevelLabel);
            sendLayout->addWidget(removeSend);
            sendsLayout->addWidget(sendRow);
            connect(sendBus, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this, trackId, sendIndex, sendBus, sendLevel](int index)
            {
                if (index >= 0)
                    setTrackSend(trackId, sendIndex, sendBus->currentData().toULongLong(),
                                 sendLevel->value() / 100.0f);
            });
            connect(sendLevel, &QSlider::valueChanged, this,
                    [this, trackId, sendIndex, sendBus, sendLevelLabel](int value)
            {
                sendLevelLabel->setText(tr("%1%").arg(value));
                setTrackSend(trackId, sendIndex, sendBus->currentData().toULongLong(), value / 100.0f);
            });
            connect(removeSend, &QPushButton::clicked, this, [this, trackId, sendIndex]
            {
                QTimer::singleShot(0, this, [this, trackId, sendIndex]
                {
                    removeTrackSend(trackId, sendIndex);
                });
            });
        }
        auto* addSendButton = new QPushButton(tr("Add auxiliary send"), sendsGroup);
        addSendButton->setEnabled(!project.buses.isEmpty() && item.sends.size() < project.buses.size());
        sendsLayout->addWidget(addSendButton);
        form->addRow(sendsGroup);
        connect(addSendButton, &QPushButton::clicked, this, [this, trackId]
        {
            QTimer::singleShot(0, this, [this, trackId] { addTrackSend(trackId); });
        });

        createEffectControls(form, group, item.effects,
            [this, commitEffectChange, trackId](qsizetype index,
                const std::function<void(MixerEffect&)>& editEffect)
            {
                InstrumentTrack* track = project.findTrack(trackId);
                if (track == nullptr || index < 0 || index >= track->effects.size()) return;
                editEffect(track->effects[index]);
                commitEffectChange();
            },
            [this, commitEffectChange, effectCount, finishTrackPluginAutomation, trackId]
            (MixerEffectType type)
            {
                QTimer::singleShot(0, this,
                    [this, commitEffectChange, effectCount, finishTrackPluginAutomation, trackId, type]
                {
                    InstrumentTrack* track = project.findTrack(trackId);
                    if (track == nullptr || track->effects.size() >= 8 || effectCount() >= 48) return;
                    finishTrackPluginAutomation(trackId);
                    MixerEffect effect;
                    QString pluginError;
                    if (!chooseMixerEffect(this, type, effect, pluginError))
                    {
                        if (!pluginError.isEmpty())
                            statusBar()->showMessage(pluginError, 5000);
                        return;
                    }
                    track->effects.push_back(std::move(effect));
                    commitEffectChange();
                    refreshMixer();
                });
            },
            [this, commitEffectChange, finishTrackPluginAutomation, trackId](qsizetype index)
            {
                QTimer::singleShot(0, this,
                    [this, commitEffectChange, finishTrackPluginAutomation, trackId, index]
                {
                    InstrumentTrack* track = project.findTrack(trackId);
                    if (track == nullptr || index < 0 || index >= track->effects.size()) return;
                    finishTrackPluginAutomation(trackId);
                    track->effects.removeAt(index);
                    commitEffectChange();
                    refreshMixer();
                });
            },
            [this, commitEffectChange, finishTrackPluginAutomation, trackId]
            (qsizetype from, qsizetype to)
            {
                QTimer::singleShot(0, this,
                    [this, commitEffectChange, finishTrackPluginAutomation, trackId, from, to]
                {
                    InstrumentTrack* track = project.findTrack(trackId);
                    if (track == nullptr || from < 0 || from >= track->effects.size() ||
                        to < 0 || to >= track->effects.size()) return;
                    finishTrackPluginAutomation(trackId);
                    track->effects.move(from, to);
                    commitEffectChange();
                    refreshMixer();
                });
            }, effectCount() < 48, trackId);

        auto* volume = new QSlider(Qt::Horizontal, group);
        volume->setObjectName(QStringLiteral("mixerTrackVolume_%1").arg(item.id));
        volume->setRange(0, 100);
        volume->setValue(static_cast<int>(std::round(item.gain * 100.0f)));
        auto* volumeLabel = new QLabel(tr("%1%").arg(volume->value()), group);
        auto* volumeRow = new QWidget(group);
        auto* volumeLayout = new QHBoxLayout(volumeRow);
        volumeLayout->setContentsMargins(0, 0, 0, 0);
        volumeLayout->addWidget(volume, 1);
        volumeLayout->addWidget(volumeLabel);
        form->addRow(tr("Level"), volumeRow);

        auto* pan = new QSlider(Qt::Horizontal, group);
        pan->setObjectName(QStringLiteral("mixerTrackPan_%1").arg(item.id));
        pan->setRange(-100, 100);
        pan->setValue(static_cast<int>(std::round(item.pan * 100.0f)));
        auto* panLabel = new QLabel(item.pan < -0.02f ? tr("L %1").arg(std::abs(pan->value())) :
                                    item.pan > 0.02f ? tr("R %1").arg(pan->value()) : tr("Center"), group);
        auto* panRow = new QWidget(group);
        auto* panLayout = new QHBoxLayout(panRow);
        panLayout->setContentsMargins(0, 0, 0, 0);
        panLayout->addWidget(pan, 1);
        panLayout->addWidget(panLabel);
        form->addRow(tr("Pan"), panRow);
        auto* peakMeter = createPeakMeter(group);
        peakMeter->setObjectName(QStringLiteral("trackPeakMeter"));
        peakMeter->setToolTip(tr("Track peak after level, pan, EQ, and inserts; before bus processing"));
        mixerTrackMeters.push_back({trackId, peakMeter});
        form->addRow(tr("Peak"), peakMeter);
        layout->addWidget(group);

        connect(volume, &QSlider::sliderPressed, this, [this, trackId, volume]
        {
            beginAutomationWrite(trackId, 0, volume->value() / 100.0f);
        });
        connect(volume, &QSlider::sliderReleased, this, [this, trackId]
        {
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 0 &&
                automationWriteModeControl->currentData().toInt() != 1)
                finishAutomationWrite();
        });
        connect(pan, &QSlider::sliderPressed, this, [this, trackId, pan]
        {
            beginAutomationWrite(trackId, 1, pan->value() / 100.0f);
        });
        connect(pan, &QSlider::sliderReleased, this, [this, trackId]
        {
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 1 &&
                automationWriteModeControl->currentData().toInt() != 1)
                finishAutomationWrite();
        });

        connect(mute, &QCheckBox::toggled, this, [this, trackId](bool enabled)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            track->muted = enabled;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(solo, &QCheckBox::toggled, this, [this, trackId](bool enabled)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            track->solo = enabled;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(volume, &QSlider::valueChanged, this, [this, trackId, volume, volumeLabel](int value)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            const float level = value / 100.0f;
            volumeLabel->setText(tr("%1%").arg(value));
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 0)
            {
                recordAutomationValue(trackId, 0, level);
                if (!volume->isSliderDown() && automationWriteModeControl->currentData().toInt() != 1)
                    automationWriteIdleTimer->start(250);
                return;
            }
            if (automationWriteAction->isChecked() && automationLaneParameterControl->currentData().toInt() == 0 &&
                audioEngine.isPlaying())
            {
                beginAutomationWrite(trackId, 0, track->gain);
                if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                    automationWriteSession.parameter == 0)
                {
                    recordAutomationValue(trackId, 0, level);
                    if (!volume->isSliderDown() && automationWriteModeControl->currentData().toInt() != 1)
                        automationWriteIdleTimer->start(250);
                    return;
                }
            }
            track->gain = value / 100.0f;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(pan, &QSlider::valueChanged, this, [this, trackId, pan, panLabel](int value)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            const float balance = value / 100.0f;
            panLabel->setText(value < -2 ? tr("L %1").arg(-value) : value > 2 ? tr("R %1").arg(value) : tr("Center"));
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 1)
            {
                recordAutomationValue(trackId, 1, balance);
                if (!pan->isSliderDown() && automationWriteModeControl->currentData().toInt() != 1)
                    automationWriteIdleTimer->start(250);
                return;
            }
            if (automationWriteAction->isChecked() && automationLaneParameterControl->currentData().toInt() == 1 &&
                audioEngine.isPlaying())
            {
                beginAutomationWrite(trackId, 1, track->pan);
                if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                    automationWriteSession.parameter == 1)
                {
                    recordAutomationValue(trackId, 1, balance);
                    if (!pan->isSliderDown() && automationWriteModeControl->currentData().toInt() != 1)
                        automationWriteIdleTimer->start(250);
                    return;
                }
            }
            track->pan = value / 100.0f;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
    }
    layout->addStretch();
    mixerScrollArea->setWidget(content);
}

void MainWindow::refreshMixerMeters()
{
    const auto meterValue = [](float peak)
    {
        if (!std::isfinite(peak) || peak <= 0.001f)
            return 0;
        const float decibels = 20.0f * std::log10(peak);
        return static_cast<int>(std::round(std::clamp((decibels + 60.0f) / 60.0f, 0.0f, 1.0f) * 1000.0f));
    };
    const auto meterTip = [this](float peak)
    {
        if (!std::isfinite(peak) || peak <= 0.0f)
            return tr("Peak: -∞ dBFS");
        return tr("Peak: %1 dBFS").arg(20.0f * std::log10(peak), 0, 'f', 1);
    };

    if (masterPeakMeter != nullptr)
    {
        const float peak = audioEngine.masterPeakLevel();
        masterPeakMeter->setValue(meterValue(peak));
        masterPeakMeter->setToolTip(meterTip(peak));
    }
    for (const MixerTrackMeterWidget& entry : mixerTrackMeters)
    {
        if (entry.meter == nullptr)
            continue;
        const float peak = audioEngine.trackPeakLevel(entry.trackId);
        entry.meter->setValue(meterValue(peak));
        entry.meter->setToolTip(meterTip(peak));
    }
    for (const MixerBusMeterWidget& entry : mixerBusMeters)
    {
        if (entry.meter == nullptr)
            continue;
        const float peak = audioEngine.busPeakLevel(entry.busIndex);
        entry.meter->setValue(meterValue(peak));
        entry.meter->setToolTip(meterTip(peak));
    }
}

void MainWindow::addMixerBus()
{
    if (project.buses.size() >= 64)
    {
        statusBar()->showMessage(tr("This project has reached the 64-bus limit"), 3000);
        return;
    }
    MixerBus bus;
    bus.id = project.nextBusId();
    bus.name = tr("Bus %1").arg(project.buses.size() + 1);
    project.buses.push_back(bus);
    project.normalize();
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Added mixer bus %1").arg(bus.name), 1800);
}

void MainWindow::removeMixerBus(quint64 busId)
{
    const auto bus = std::find_if(project.buses.begin(), project.buses.end(), [busId](const MixerBus& item)
    {
        return item.id == busId;
    });
    if (bus == project.buses.end())
        return;
    const QString name = bus->name;
    const quint64 parentBusId = bus->outputBusId;
    QString parentName = tr("Master");
    for (const MixerBus& candidate : project.buses)
    {
        if (candidate.id == parentBusId)
        {
            parentName = candidate.name;
            break;
        }
    }
    for (MixerBus& candidate : project.buses)
    {
        if (candidate.id != busId)
        {
            for (MixerSend& send : candidate.sends)
            {
                if (send.busId == busId)
                    send.busId = parentBusId;
            }
            candidate.sends.erase(std::remove_if(candidate.sends.begin(), candidate.sends.end(),
                [](const MixerSend& send) { return send.busId == 0; }), candidate.sends.end());
        }
        if (candidate.outputBusId == busId)
            candidate.outputBusId = parentBusId;
    }
    for (InstrumentTrack& track : project.tracks)
    {
        if (track.outputBusId == busId)
            track.outputBusId = parentBusId;
        for (MixerSend& send : track.sends)
        {
            if (send.busId == busId)
                send.busId = parentBusId;
        }
        track.sends.erase(std::remove_if(track.sends.begin(), track.sends.end(),
            [](const MixerSend& send) { return send.busId == 0; }), track.sends.end());
    }
    project.buses.erase(bus);
    project.normalize();
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Removed %1; rerouted its outputs to %2").arg(name, parentName), 2200);
}

bool MainWindow::mixerRouteWouldCycle(quint64 sourceBusId, quint64 targetBusId) const
{
    if (targetBusId == 0)
        return false;
    if (sourceBusId == targetBusId)
        return true;

    std::vector<quint64> pending{targetBusId};
    std::unordered_set<quint64> visited;
    while (!pending.empty())
    {
        const quint64 routeBusId = pending.back();
        pending.pop_back();
        if (routeBusId == sourceBusId)
            return true;
        if (!visited.insert(routeBusId).second)
            continue;
        const auto routedBus = std::find_if(project.buses.cbegin(), project.buses.cend(),
            [routeBusId](const MixerBus& candidate) { return candidate.id == routeBusId; });
        if (routedBus == project.buses.cend())
            continue;
        if (routedBus->outputBusId != 0)
            pending.push_back(routedBus->outputBusId);
        for (const MixerSend& send : routedBus->sends)
            pending.push_back(send.busId);
    }
    return false;
}

void MainWindow::setMixerBusOutput(quint64 busId, quint64 outputBusId)
{
    MixerBus* bus = nullptr;
    for (MixerBus& candidate : project.buses)
    {
        if (candidate.id == busId)
        {
            bus = &candidate;
            break;
        }
    }
    if (bus == nullptr || bus->outputBusId == outputBusId)
        return;

    if (mixerRouteWouldCycle(busId, outputBusId))
    {
        statusBar()->showMessage(tr("That route would create a mixer cycle"), 3000);
        QTimer::singleShot(0, this, [this] { refreshMixer(); });
        return;
    }

    bus->outputBusId = outputBusId;
    project.normalize();
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
    refreshMixer();
}

void MainWindow::setMixerBusSend(quint64 busId, qsizetype sendIndex, quint64 targetBusId, float gain)
{
    MixerBus* bus = nullptr;
    for (MixerBus& candidate : project.buses)
    {
        if (candidate.id == busId)
        {
            bus = &candidate;
            break;
        }
    }
    if (bus == nullptr || sendIndex < 0 || sendIndex >= bus->sends.size() || targetBusId == 0)
        return;
    const float normalizedGain = std::clamp(gain, 0.0f, 1.0f);
    MixerSend& send = bus->sends[sendIndex];
    if (send.busId == targetBusId && std::abs(send.gain - normalizedGain) < 1.0e-6f)
        return;
    for (qsizetype otherIndex = 0; otherIndex < bus->sends.size(); ++otherIndex)
    {
        if (otherIndex != sendIndex && bus->sends[otherIndex].busId == targetBusId)
        {
            statusBar()->showMessage(tr("A bus can send to a destination only once"), 2500);
            QTimer::singleShot(0, this, [this] { refreshMixer(); });
            return;
        }
    }
    if (mixerRouteWouldCycle(busId, targetBusId))
    {
        statusBar()->showMessage(tr("That send would create a mixer cycle"), 3000);
        QTimer::singleShot(0, this, [this] { refreshMixer(); });
        return;
    }
    const bool destinationChanged = send.busId != targetBusId;
    send.busId = targetBusId;
    send.gain = normalizedGain;
    project.normalize();
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
    if (destinationChanged)
        QTimer::singleShot(0, this, [this] { refreshMixer(); });
}

void MainWindow::addMixerBusSend(quint64 busId)
{
    MixerBus* bus = nullptr;
    for (MixerBus& candidate : project.buses)
    {
        if (candidate.id == busId)
        {
            bus = &candidate;
            break;
        }
    }
    if (bus == nullptr || bus->sends.size() >= project.buses.size() - 1)
        return;
    for (const MixerBus& candidate : project.buses)
    {
        if (candidate.id == busId || mixerRouteWouldCycle(busId, candidate.id))
            continue;
        const bool alreadyUsed = std::any_of(bus->sends.cbegin(), bus->sends.cend(),
            [&candidate](const MixerSend& send) { return send.busId == candidate.id; });
        if (alreadyUsed)
            continue;
        bus->sends.push_back({candidate.id, 0.5f});
        project.normalize();
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
        refreshMixer();
        return;
    }
    statusBar()->showMessage(tr("No cycle-free destination is available for this bus"), 3000);
}

void MainWindow::removeMixerBusSend(quint64 busId, qsizetype sendIndex)
{
    for (MixerBus& bus : project.buses)
    {
        if (bus.id != busId || sendIndex < 0 || sendIndex >= bus.sends.size())
            continue;
        bus.sends.removeAt(sendIndex);
        project.normalize();
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
        refreshMixer();
        return;
    }
}

void MainWindow::setTrackOutputBus(quint64 trackId, quint64 busId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr || track->outputBusId == busId)
        return;
    track->outputBusId = busId;
    project.normalize();
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setTrackSend(quint64 trackId, qsizetype sendIndex, quint64 busId, float gain)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr || sendIndex < 0 || sendIndex >= track->sends.size() || busId == 0)
        return;
    const float normalizedGain = std::clamp(gain, 0.0f, 1.0f);
    MixerSend& send = track->sends[sendIndex];
    if (send.busId == busId && std::abs(send.gain - normalizedGain) < 1.0e-6f)
        return;
    for (qsizetype otherIndex = 0; otherIndex < track->sends.size(); ++otherIndex)
    {
        if (otherIndex != sendIndex && track->sends[otherIndex].busId == busId)
        {
            statusBar()->showMessage(tr("Each track can send to a bus only once"), 2500);
            QTimer::singleShot(0, this, [this] { refreshMixer(); });
            return;
        }
    }
    send.busId = busId;
    send.gain = normalizedGain;
    project.normalize();
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::addTrackSend(quint64 trackId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr || track->sends.size() >= project.buses.size())
        return;
    const auto bus = std::find_if(project.buses.cbegin(), project.buses.cend(), [track](const MixerBus& item)
    {
        return std::none_of(track->sends.cbegin(), track->sends.cend(), [&item](const MixerSend& send)
        {
            return send.busId == item.id;
        });
    });
    if (bus == project.buses.cend())
        return;
    track->sends.push_back({bus->id, 0.5f});
    project.normalize();
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshProjectUi();
}

void MainWindow::removeTrackSend(quint64 trackId, qsizetype sendIndex)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr || sendIndex < 0 || sendIndex >= track->sends.size())
        return;
    track->sends.removeAt(sendIndex);
    project.normalize();
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshProjectUi();
}

void MainWindow::selectTrack(quint64 trackId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    selectedTrackId = trackId;
    selectedClipId = track->clips.isEmpty() ? 0 : track->clips.first().id;
    refreshProjectUi();
    if (track->instrumentType == InstrumentType::DrumRack)
        editorTabs->setCurrentWidget(stepSequencer);
    else if (track->instrumentType == InstrumentType::Audio)
        editorTabs->setCurrentWidget(arrangementEditor);
}

void MainWindow::selectClip(quint64 trackId, quint64 clipId, bool openPianoRoll)
{
    if (project.findClip(trackId, clipId) == nullptr)
        return;
    selectedTrackId = trackId;
    selectedClipId = clipId;
    refreshProjectUi();
    if (openPianoRoll)
    {
        const InstrumentTrack* track = project.findTrack(trackId);
        if (track != nullptr && track->instrumentType == InstrumentType::Audio)
            editorTabs->setCurrentWidget(arrangementEditor);
        else
            editorTabs->setCurrentWidget(track != nullptr && track->instrumentType == InstrumentType::DrumRack
                                             ? static_cast<QWidget*>(stepSequencer)
                                             : static_cast<QWidget*>(midiEditor));
    }
}

void MainWindow::addTrack()
{
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("Synth %1").arg(track.id);
    project.tracks.push_back(track);
    selectedTrackId = track.id;
    selectedClipId = 0;
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Added %1").arg(track.name), 1800);
}

void MainWindow::addDrumTrack()
{
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("Drum Rack %1").arg(track.id);
    track.instrumentType = InstrumentType::DrumRack;
    track.gain = 0.8f;
    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = tr("Beat %1").arg(clip.id);
    clip.lengthBeats = project.patternLengthBeats();
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(stepSequencer);
    statusBar()->showMessage(tr("Added a drum rack with kick, snare, closed-hat, and open-hat sounds"), 2500);
}

void MainWindow::addClapInstrumentTrack()
{
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }

    const QVector<ClapPluginDescriptor> plugins = ClapAudioPluginInstance::availablePlugins(
        ClapPluginKind::Instrument);
    if (plugins.isEmpty())
    {
        QMessageBox::information(this, tr("Add CLAP instrument"),
            tr("No supported CLAP instruments were found in the standard plug-in folders or CLAP_PATH."));
        return;
    }

    QStringList choices;
    choices.reserve(plugins.size());
    for (const ClapPluginDescriptor& plugin : plugins)
        choices.push_back(QStringLiteral("%1 — %2 — %3")
                              .arg(plugin.name, plugin.vendor, plugin.id));
    bool accepted = false;
    const QString selected = QInputDialog::getItem(this, tr("Add CLAP instrument"),
        tr("Instrument"), choices, 0, false, &accepted);
    if (!accepted)
        return;
    const int index = choices.indexOf(selected);
    if (index < 0)
        return;

    const ClapPluginDescriptor& plugin = plugins[index];
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("%1 %2").arg(plugin.name).arg(track.id);
    track.instrumentType = InstrumentType::ClapInstrument;
    track.clapPluginId = plugin.id;
    track.clapPluginName = plugin.name;
    track.clapParameterValues.reserve(plugin.parameters.size());
    for (const ClapPluginParameter& parameter : plugin.parameters)
        track.clapParameterValues.push_back(parameter.defaultValue);

    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = tr("Pattern %1").arg(clip.id);
    clip.lengthBeats = project.patternLengthBeats();
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(midiEditor);
    statusBar()->showMessage(tr("Added CLAP instrument: %1").arg(plugin.name), 2500);
}

void MainWindow::editSelectedClapInstrument()
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::ClapInstrument)
        return;

    ClapPluginDescriptor descriptor;
    if (!ClapAudioPluginInstance::describe(track->clapPluginId,
                                           ClapPluginKind::Instrument, descriptor))
    {
        QMessageBox::warning(this, tr("CLAP instrument unavailable"),
            tr("Install %1 or choose another instrument to edit its parameters.")
                .arg(track->clapPluginName.isEmpty() ? track->name : track->clapPluginName));
        return;
    }
    const quint64 trackId = track->id;
    const QString instrumentPluginId = track->clapPluginId;

    struct ParameterControl
    {
        qsizetype index = 0;
        QWidget* editor = nullptr;
    };

    QDialog dialog(this);
    dialog.setWindowTitle(tr("%1 controls").arg(descriptor.name));
    auto* layout = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto* formContents = new QWidget(scroll);
    auto* form = new QFormLayout(formContents);
    QVector<float> values = track->clapParameterValues;
    if (values.size() > descriptor.parameters.size())
        values.resize(descriptor.parameters.size());
    while (values.size() < descriptor.parameters.size())
        values.push_back(descriptor.parameters[values.size()].defaultValue);

    QVector<ParameterControl> controls;
    for (qsizetype index = 0; index < descriptor.parameters.size(); ++index)
    {
        const ClapPluginParameter& parameter = descriptor.parameters[index];
        if (parameter.hidden)
            continue;
        const float value = std::clamp(values[index], parameter.minimum, parameter.maximum);
        const int automationTarget = clapInstrumentAutomationTarget(index);
        const auto recordInstrumentParameterChange =
            [this, trackId, instrumentPluginId, automationTarget, index, parameter](float newValue)
        {
            if (automationWriteSession.active &&
                automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == automationTarget &&
                automationWriteSession.pluginUri == instrumentPluginId)
            {
                recordAutomationValue(trackId, automationTarget, newValue);
                if (automationWriteModeControl->currentData().toInt() != 1)
                    automationWriteIdleTimer->start(250);
                return;
            }
            if (automationWriteAction == nullptr || !automationWriteAction->isChecked() ||
                !audioEngine.isPlaying() || automationLaneParameterControl == nullptr ||
                automationLaneParameterControl->currentData().toInt() != automationTarget)
                return;
            InstrumentTrack* currentTrack = project.findTrack(trackId);
            if (currentTrack == nullptr ||
                currentTrack->instrumentType != InstrumentType::ClapInstrument ||
                currentTrack->clapPluginId != instrumentPluginId)
                return;
            const float baseValue = index < currentTrack->clapParameterValues.size()
                ? currentTrack->clapParameterValues[index] : parameter.defaultValue;
            beginAutomationWrite(trackId, automationTarget, baseValue, instrumentPluginId,
                                 -1, index, parameter.minimum, parameter.maximum,
                                 parameter.name);
            if (automationWriteSession.active &&
                automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == automationTarget &&
                automationWriteSession.pluginUri == instrumentPluginId)
            {
                recordAutomationValue(trackId, automationTarget, newValue);
                if (automationWriteModeControl->currentData().toInt() != 1)
                    automationWriteIdleTimer->start(250);
            }
        };
        if (parameter.toggled)
        {
            auto* editor = new QCheckBox(formContents);
            editor->setChecked(value >= (parameter.minimum + parameter.maximum) * 0.5f);
            editor->setEnabled(!parameter.readOnly);
            form->addRow(parameter.name, editor);
            connect(editor, &QCheckBox::toggled, this,
                    [parameter, recordInstrumentParameterChange](bool checked)
            {
                recordInstrumentParameterChange(checked ? parameter.maximum : parameter.minimum);
            });
            controls.push_back({index, editor});
        }
        else
        {
            auto* editor = new QDoubleSpinBox(formContents);
            editor->setRange(parameter.minimum, parameter.maximum);
            const bool wholeNumberRange = std::floor(parameter.minimum) == parameter.minimum &&
                std::floor(parameter.maximum) == parameter.maximum;
            editor->setDecimals(parameter.integer && wholeNumberRange ? 0 : 3);
            editor->setSingleStep(parameter.integer ? 1.0 :
                std::max(0.001, static_cast<double>(parameter.maximum - parameter.minimum) / 100.0));
            editor->setValue(value);
            editor->setEnabled(!parameter.readOnly);
            form->addRow(parameter.name, editor);
            connect(editor, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                    [recordInstrumentParameterChange](double newValue)
            {
                recordInstrumentParameterChange(static_cast<float>(newValue));
            });
            controls.push_back({index, editor});
        }
    }
    scroll->setWidget(formContents);
    layout->addWidget(scroll);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(440, 520);
    if (controls.isEmpty())
    {
        QMessageBox::information(this, tr("CLAP instrument controls"),
            tr("This instrument does not expose editable parameters."));
        return;
    }
    if (dialog.exec() != QDialog::Accepted)
        return;

    audioEngine.capturePluginStates(project);
    track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    for (const ParameterControl& control : controls)
    {
        if (auto* toggle = qobject_cast<QCheckBox*>(control.editor))
        {
            const ClapPluginParameter& parameter = descriptor.parameters[control.index];
            values[control.index] = toggle->isChecked() ? parameter.maximum : parameter.minimum;
        }
        else if (auto* number = qobject_cast<QDoubleSpinBox*>(control.editor))
        {
            values[control.index] = static_cast<float>(number->value());
        }
    }
    track->clapParameterValues = std::move(values);
    project.normalize();
    settingsDirty = true;
    refreshProjectUi();
}

void MainWindow::addClipAt(quint64 trackId, double startBeat)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    if (track->instrumentType == InstrumentType::Audio)
    {
        statusBar()->showMessage(tr("Import a WAV file to add an audio region to this track"), 2500);
        return;
    }
    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = tr("Pattern %1").arg(clip.id);
    clip.startBeat = std::max(0.0, startBeat);
    clip.lengthBeats = project.patternLengthBeats();
    track->clips.push_back(clip);
    selectedTrackId = trackId;
    selectedClipId = clip.id;
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Added %1 to the Playlist").arg(clip.name), 1800);
}

void MainWindow::addNewPattern()
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
    {
        addTrack();
        track = selectedTrack();
    }
    if (track == nullptr)
        return;
    if (track->instrumentType == InstrumentType::Audio)
    {
        statusBar()->showMessage(tr("Use Import WAV audio to add another audio region"), 2500);
        return;
    }

    double startBeat = 0.0;
    for (const MidiClip& clip : track->clips)
        startBeat = std::max(startBeat, clip.startBeat + clip.lengthBeats);
    const double barLength = project.quarterNotesPerBar();
    startBeat = std::ceil(startBeat / barLength) * barLength;
    addClipAt(track->id, startBeat);
}

void MainWindow::removeClip(quint64 trackId, quint64 clipId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    for (auto it = track->clips.begin(); it != track->clips.end(); ++it)
    {
        if (it->id == clipId)
        {
            track->clips.erase(it);
            if (selectedTrackId == trackId && selectedClipId == clipId)
                selectedClipId = track->clips.isEmpty() ? 0 : track->clips.first().id;
            settingsDirty = true;
            refreshProjectUi();
            statusBar()->showMessage(tr("Pattern removed"), 1600);
            return;
        }
    }
}

void MainWindow::moveClip(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat)
{
    InstrumentTrack* source = project.findTrack(sourceTrackId);
    InstrumentTrack* target = project.findTrack(targetTrackId);
    if (source == nullptr || target == nullptr)
        return;
    const MidiClip* sourceClip = project.findClip(sourceTrackId, clipId);
    if (sourceClip == nullptr || sourceClip->isAudioRegion() !=
        (target->instrumentType == InstrumentType::Audio))
    {
        statusBar()->showMessage(tr("Audio regions stay on audio tracks; MIDI patterns stay on instrument tracks"), 2500);
        return;
    }
    MidiClip movedClip;
    bool found = false;
    for (auto it = source->clips.begin(); it != source->clips.end(); ++it)
    {
        if (it->id == clipId)
        {
            movedClip = *it;
            source->clips.erase(it);
            found = true;
            break;
        }
    }
    if (!found)
        return;
    movedClip.startBeat = std::clamp(startBeat, 0.0, 100000.0);
    target->clips.push_back(movedClip);
    selectedTrackId = targetTrackId;
    selectedClipId = clipId;
    settingsDirty = true;
    refreshProjectUi();
    refreshDirtyState();
}

void MainWindow::trimAudioRegion(quint64 trackId, quint64 clipId, double startBeat,
                                 double lengthBeats, qint64 audioStartFrame)
{
    InstrumentTrack* track = project.findTrack(trackId);
    MidiClip* clip = project.findClip(trackId, clipId);
    if (track == nullptr || clip == nullptr || track->instrumentType != InstrumentType::Audio ||
        !clip->isAudioRegion())
        return;

    clip->startBeat = std::clamp(startBeat, 0.0, 16384.0);
    clip->lengthBeats = std::clamp(lengthBeats, 0.25, 16384.0);
    clip->audioStartFrame = std::max<qint64>(0, audioStartFrame);
    project.normalize();
    settingsDirty = true;
    refreshProjectUi();
    refreshDirtyState();
    statusBar()->showMessage(tr("Audio region trimmed"), 1800);
}

void MainWindow::relinkAudioRegion(quint64 trackId, quint64 clipId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    MidiClip* clip = project.findClip(trackId, clipId);
    if (track == nullptr || clip == nullptr || track->instrumentType != InstrumentType::Audio ||
        !clip->isAudioRegion())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Relink audio region"),
        QFileInfo(clip->audioSourcePath).absolutePath(), tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset = findProjectAudioAsset(path);
    QString error;
    if (asset == nullptr && !AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not relink audio region"), error);
        return;
    }

    const QString previousPath = clip->audioSourcePath;
    const std::shared_ptr<const AudioAsset> previousSource = clip->audioSource;
    const qint64 previousStartFrame = clip->audioStartFrame;
    clip->audioSourcePath = asset->path;
    clip->audioSource = asset;
    clip->audioStartFrame = std::clamp<qint64>(clip->audioStartFrame, 0,
                                               std::max<qint64>(0, asset->frameCount() - 1));
    if (projectAudioBytes() > AudioFile::maximumProjectDecodedBytes)
    {
        clip->audioSourcePath = previousPath;
        clip->audioSource = previousSource;
        clip->audioStartFrame = previousStartFrame;
        QMessageBox::warning(this, tr("Audio memory limit"),
                             tr("This project can load up to 512 MiB of decoded audio. Remove another audio source first."));
        return;
    }

    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Relinked audio region to %1").arg(QFileInfo(path).fileName()), 3000);
}

void MainWindow::setSelectedAudioFade(bool fadeIn, double beats)
{
    MidiClip* clip = selectedClip();
    if (clip == nullptr || !clip->isAudioRegion())
        return;

    const double fadeBeats = std::clamp(beats, 0.0, clip->lengthBeats);
    if (fadeIn)
        clip->audioFadeInBeats = fadeBeats;
    else
        clip->audioFadeOutBeats = fadeBeats;
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedAudioFadeCurve(bool fadeIn, int curve)
{
    MidiClip* clip = selectedClip();
    if (clip == nullptr || !clip->isAudioRegion() || curve < 0 || curve > 3)
        return;
    const auto fadeCurve = static_cast<AudioFadeCurve>(curve);
    AudioFadeCurve& current = fadeIn ? clip->audioFadeInCurve : clip->audioFadeOutCurve;
    if (current == fadeCurve)
        return;
    current = fadeCurve;
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedAudioFadeShape(bool fadeIn, double shape)
{
    MidiClip* clip = selectedClip();
    if (clip == nullptr || !clip->isAudioRegion() || !std::isfinite(shape))
        return;
    const double normalizedShape = std::clamp(shape, 0.25, 4.0);
    double& current = fadeIn ? clip->audioFadeInShape : clip->audioFadeOutShape;
    if (std::abs(current - normalizedShape) < 1.0e-6)
        return;
    current = normalizedShape;
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::createSelectedAudioCrossfade()
{
    InstrumentTrack* track = selectedTrack();
    MidiClip* selected = selectedClip();
    if (track == nullptr || selected == nullptr || !selected->isAudioRegion())
        return;

    const QVector<const MidiClip*> candidates = overlappingAudioRegions(project, *track, *selected);
    if (candidates.isEmpty())
    {
        statusBar()->showMessage(tr("Load an overlapping audio region on this track to create a crossfade"), 3000);
        return;
    }

    int candidateIndex = 0;
    if (candidates.size() > 1)
    {
        QStringList choices;
        for (const MidiClip* candidate : candidates)
        {
            choices.push_back(tr("%1  ·  beats %2–%3")
                .arg(candidate->name)
                .arg(candidate->startBeat + 1.0, 0, 'f', 2)
                .arg(audioRegionEndBeat(project, *candidate) + 1.0, 0, 'f', 2) +
                tr("  ·  clip %1").arg(candidate->id));
        }
        bool accepted = false;
        const QString choice = QInputDialog::getItem(this, tr("Choose overlapping audio region"),
                                                      tr("Crossfade with"), choices, 0, false, &accepted);
        if (!accepted)
            return;
        candidateIndex = choices.indexOf(choice);
        if (candidateIndex < 0)
            return;
    }

    const MidiClip* partner = candidates[candidateIndex];
    const double overlapStart = std::max(selected->startBeat, partner->startBeat);
    const double overlapEnd = std::min(audioRegionEndBeat(project, *selected),
                                       audioRegionEndBeat(project, *partner));
    const double overlapBeats = overlapEnd - overlapStart;
    if (overlapBeats <= 1.0e-6)
        return;

    const MidiClip* outgoing = selected->startBeat < partner->startBeat ? selected : partner;
    const MidiClip* incoming = outgoing == selected ? partner : selected;
    QVector<AudioFadeState> before{
        {outgoing->id, outgoing->audioFadeInBeats, outgoing->audioFadeOutBeats,
         outgoing->audioFadeInCurve, outgoing->audioFadeOutCurve,
         outgoing->audioFadeInShape, outgoing->audioFadeOutShape},
        {incoming->id, incoming->audioFadeInBeats, incoming->audioFadeOutBeats,
         incoming->audioFadeInCurve, incoming->audioFadeOutCurve,
         incoming->audioFadeInShape, incoming->audioFadeOutShape}
    };
    QVector<AudioFadeState> after = before;
    after[0].fadeOutBeats = overlapBeats;
    after[1].fadeInBeats = overlapBeats;
    after[0].fadeOutCurve = AudioFadeCurve::EqualPower;
    after[1].fadeInCurve = AudioFadeCurve::EqualPower;
    undoStack.push(new AudioFadeEditCommand(this, track->id, before, after));
    statusBar()->showMessage(tr("Created an equal-power crossfade over %1 beats")
                                 .arg(overlapBeats, 0, 'f', 2), 2500);
}

void MainWindow::applyAudioFadeHistory(quint64 trackId, const QVector<AudioFadeState>& fades)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    for (const AudioFadeState& fade : fades)
    {
        MidiClip* clip = project.findClip(trackId, fade.clipId);
        if (clip == nullptr || !clip->isAudioRegion())
            continue;
        clip->audioFadeInBeats = fade.fadeInBeats;
        clip->audioFadeOutBeats = fade.fadeOutBeats;
        clip->audioFadeInCurve = fade.fadeInCurve;
        clip->audioFadeOutCurve = fade.fadeOutCurve;
        clip->audioFadeInShape = fade.fadeInShape;
        clip->audioFadeOutShape = fade.fadeOutShape;
    }
    project.normalize();
    audioEngine.setProject(project);
    refreshProjectUi();
    refreshDirtyState();
}

void MainWindow::setSelectedSynthEnvelope(int parameter, double value)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::Synth)
        return;
    if (synthPresetControl != nullptr)
        synthPresetControl->setCurrentIndex(-1);
    switch (parameter)
    {
        case 0: track->attackSeconds = static_cast<float>(value); break;
        case 1: track->decaySeconds = static_cast<float>(value); break;
        case 2: track->sustainLevel = static_cast<float>(value); break;
        case 3: track->releaseSeconds = static_cast<float>(value); break;
        default: return;
    }
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedSynthOscillator(int parameter, double value)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::Synth)
        return;
    if (synthPresetControl != nullptr)
        synthPresetControl->setCurrentIndex(-1);
    switch (parameter)
    {
        case 0: track->secondOscillatorEnabled = value >= 0.5; break;
        case 1: track->secondOscillatorWaveform = static_cast<SynthWaveform>(
                    std::clamp(static_cast<int>(value), 0, 3)); break;
        case 2: track->secondOscillatorDetuneCents = static_cast<float>(
                    std::clamp(value, -100.0, 100.0)); break;
        case 3: track->secondOscillatorMix = static_cast<float>(
                    std::clamp(value / 100.0, 0.0, 1.0)); break;
        default: return;
    }
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedSynthFilter(int parameter, double value)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::Synth)
        return;
    if (synthPresetControl != nullptr)
        synthPresetControl->setCurrentIndex(-1);
    switch (parameter)
    {
        case 0: track->synthFilterEnabled = value >= 0.5; break;
        case 1: track->synthFilterCutoffHz = static_cast<float>(std::clamp(value, 20.0, 20000.0)); break;
        case 2: track->synthFilterResonance = static_cast<float>(std::clamp(value, 0.1, 10.0)); break;
        case 3: track->synthFilterType = static_cast<SynthFilterType>(
                    std::clamp(static_cast<int>(value), 0, 3)); break;
        default: return;
    }
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedSynthFilterEnvelope(int parameter, double value)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::Synth)
        return;
    if (synthPresetControl != nullptr)
        synthPresetControl->setCurrentIndex(-1);
    switch (parameter)
    {
        case 0: track->synthFilterEnvelopeEnabled = value >= 0.5; break;
        case 1: track->synthFilterEnvelopeAmountSemitones = static_cast<float>(
                    std::clamp(value, -48.0, 48.0)); break;
        case 2: track->synthFilterEnvelopeAttackSeconds = static_cast<float>(
                    std::clamp(value, 0.0, 5.0)); break;
        case 3: track->synthFilterEnvelopeDecaySeconds = static_cast<float>(
                    std::clamp(value, 0.0, 5.0)); break;
        case 4: track->synthFilterEnvelopeSustainLevel = static_cast<float>(
                    std::clamp(value, 0.0, 1.0)); break;
        case 5: track->synthFilterEnvelopeReleaseSeconds = static_cast<float>(
                    std::clamp(value, 0.0, 5.0)); break;
        default: return;
    }
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::applySelectedSynthPreset(int presetIndex)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::Synth ||
        presetIndex < 0 || presetIndex >= static_cast<int>(factorySynthPresets.size()))
        return;

    applyFactorySynthPreset(*track, factorySynthPresets[static_cast<size_t>(presetIndex)]);
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshProjectUi();
    refreshDirtyState();
}

void MainWindow::setSelectedTrackEqualizer(int band, double gainDb)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || band < 0 || band > 2)
        return;
    const float normalizedGain = static_cast<float>(std::clamp(gainDb, -18.0, 18.0));
    switch (band)
    {
        case 0: track->eqLowDb = normalizedGain; break;
        case 1: track->eqMidDb = normalizedGain; break;
        case 2: track->eqHighDb = normalizedGain; break;
        default: return;
    }
    audioEngine.setProject(project);
    refreshParametricEqualizerControls();
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedTrackEqualizerCrossover(int band, double frequencyHz)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || band < 0 || band > 1)
        return;
    if (band == 0)
        track->eqLowCrossoverHz = static_cast<float>(std::clamp(
            frequencyHz, 20.0, std::min(2000.0, static_cast<double>(track->eqHighCrossoverHz) - 1.0)));
    else
        track->eqHighCrossoverHz = static_cast<float>(std::clamp(
            frequencyHz, std::max(500.0, static_cast<double>(track->eqLowCrossoverHz) + 1.0), 20000.0));
    const QSignalBlocker lowBlocker(equalizerCrossoverControls[0]);
    const QSignalBlocker highBlocker(equalizerCrossoverControls[1]);
    equalizerCrossoverControls[0]->setRange(
        20.0, std::min(2000.0, static_cast<double>(track->eqHighCrossoverHz) - 1.0));
    equalizerCrossoverControls[1]->setRange(
        std::max(500.0, static_cast<double>(track->eqLowCrossoverHz) + 1.0), 20000.0);
    equalizerCrossoverControls[0]->setValue(track->eqLowCrossoverHz);
    equalizerCrossoverControls[1]->setValue(track->eqHighCrossoverHz);
    audioEngine.setProject(project);
    refreshParametricEqualizerControls();
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedTrackParametricEqualizer(int band, const ParametricEqBand& settings)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || band < 0 || band >= static_cast<int>(track->eqParametricBands.size()))
        return;
    ParametricEqBand& target = track->eqParametricBands[static_cast<size_t>(band)];
    target.frequencyHz = std::clamp(settings.frequencyHz, 20.0f, 20000.0f);
    target.gainDb = std::clamp(settings.gainDb, -18.0f, 18.0f);
    target.q = std::clamp(settings.q, 0.1f, 10.0f);
    audioEngine.setProject(project);
    refreshParametricEqualizerControls();
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::refreshParametricEqualizerControls()
{
    if (parametricEqBandControl == nullptr || equalizerResponseGraph == nullptr)
        return;
    InstrumentTrack* track = selectedTrack();
    const int band = std::clamp(parametricEqBandControl->currentData().toInt(), 0, 2);
    const std::array<ParametricEqBand, 3> defaults{{
        {120.0f, 0.0f, 0.707f}, {1000.0f, 0.0f, 0.707f}, {8000.0f, 0.0f, 0.707f}
    }};
    const std::array<ParametricEqBand, 3>& bands = track == nullptr
        ? defaults : track->eqParametricBands;
    const ParametricEqBand& selectedBand = bands[static_cast<size_t>(band)];
    const std::array<double, 3> values = {
        selectedBand.frequencyHz, selectedBand.gainDb, selectedBand.q
    };
    for (size_t parameter = 0; parameter < parametricEqBandParameterControls.size(); ++parameter)
    {
        QDoubleSpinBox* control = parametricEqBandParameterControls[parameter];
        const QSignalBlocker blocker(control);
        control->setValue(values[parameter]);
        control->setEnabled(track != nullptr);
    }
    parametricEqBandControl->setEnabled(track != nullptr);
    auto* graph = static_cast<TrackEqResponseWidget*>(equalizerResponseGraph);
    graph->setSelectedBand(band);
    graph->setBands(bands);
    graph->setLegacyParameters(track == nullptr ? 0.0f : track->eqLowDb,
                               track == nullptr ? 0.0f : track->eqMidDb,
                               track == nullptr ? 0.0f : track->eqHighDb,
                               track == nullptr ? 120.0f : track->eqLowCrossoverHz,
                               track == nullptr ? 6000.0f : track->eqHighCrossoverHz);
    graph->setEnabled(track != nullptr);
}

void MainWindow::refreshAutomationEditor()
{
    if (automationPointsList == nullptr)
        return;
    InstrumentTrack* track = selectedTrack();
    populateAutomationTargets(automationParameterControl, track);
    populateAutomationTargets(automationLaneParameterControl, track);
    const int target = automationParameterControl->currentData().toInt();
    const bool volume = target == 0;
    const bool pan = target == 1;
    const QString pluginUri = automationParameterControl->currentData(Qt::UserRole + 4).toString();
    const QVector<AutomationPoint>* points = track == nullptr ? nullptr :
        automationCurve(static_cast<const InstrumentTrack&>(*track), target, pluginUri);
    const int targetIndex = automationParameterControl->currentIndex();
    const double minimum = automationParameterControl->itemData(targetIndex, Qt::UserRole + 1).toDouble();
    const double maximum = automationParameterControl->itemData(targetIndex, Qt::UserRole + 2).toDouble();
    automationEditorPanel->setEnabled(track != nullptr);
    automationValueLabel->setText(volume ? tr("Volume (%)") : pan ? tr("Pan (%)") :
        tr("%1 value").arg(automationParameterControl->currentText()));
    automationValueControl->setRange(volume || pan ? minimum * 100.0 : minimum,
                                     volume || pan ? maximum * 100.0 : maximum);
    automationValueControl->setSingleStep(volume ? 5.0 : pan ? 10.0 :
        std::max(0.001, (maximum - minimum) / 100.0));
    automationValueControl->setDecimals(volume || pan ? 0 : 4);
    automationValueControl->setSuffix(volume || pan ? QStringLiteral("%") : QString());

    const double selectedBeat = automationPointsList->currentItem() == nullptr ? -1.0 :
        automationPointsList->currentItem()->data(Qt::UserRole).toDouble();
    int restoredRow = -1;
    {
        const QSignalBlocker listBlocker(automationPointsList);
        automationPointsList->clear();
        if (points != nullptr)
        {
            for (const AutomationPoint& point : *points)
            {
                auto* item = new QListWidgetItem(automationPointsList);
                const double displayedValue = volume || pan ? point.value * 100.0 : point.value;
                const QString valueText = volume ? tr("%1%").arg(displayedValue, 0, 'f', 0) :
                    pan ? displayedValue < -1.0 ? tr("L %1").arg(std::abs(displayedValue), 0, 'f', 0) :
                        displayedValue > 1.0 ? tr("R %1").arg(displayedValue, 0, 'f', 0) : tr("Center") :
                        QString::number(displayedValue, 'g', 6);
                item->setText(tr("Beat %1  ·  %2  ·  %3")
                                  .arg(point.beat, 0, 'f', 2).arg(valueText)
                                  .arg(automationCurveName(point.curveToNext)));
                item->setData(Qt::UserRole, point.beat);
                item->setData(Qt::UserRole + 1, displayedValue);
                item->setData(Qt::UserRole + 2, static_cast<int>(point.curveToNext));
                if (selectedBeat >= 0.0 && std::abs(point.beat - selectedBeat) < 1.0e-9)
                    restoredRow = automationPointsList->count() - 1;
            }
        }
        if (restoredRow < 0)
            automationPointsList->setCurrentRow(-1);
    }
    if (restoredRow >= 0)
        automationPointsList->setCurrentRow(restoredRow);
    automationBeatControl->setEnabled(track != nullptr);
    automationValueControl->setEnabled(track != nullptr);
    automationCurveControl->setEnabled(track != nullptr);
    removeAutomationPointButton->setEnabled(false);
    refreshArrangementAutomationView();
}

void MainWindow::refreshArrangementAutomationView()
{
    if (automationLaneParameterControl == nullptr || automationCurveControl == nullptr ||
        arrangementEditor == nullptr)
        return;
    const int index = automationLaneParameterControl->currentIndex();
    const int target = automationLaneParameterControl->itemData(index).toInt();
    const float minimum = automationLaneParameterControl->itemData(index, Qt::UserRole + 1).toFloat();
    const float maximum = automationLaneParameterControl->itemData(index, Qt::UserRole + 2).toFloat();
    const float defaultValue = automationLaneParameterControl->itemData(index, Qt::UserRole + 3).toFloat();
    const QString pluginUri = automationLaneParameterControl->itemData(index, Qt::UserRole + 4).toString();
    const bool writeAutomationTarget = target == 0 || target == 1 || !pluginUri.isEmpty();
    if (!writeAutomationTarget && automationWriteAction != nullptr && automationWriteAction->isChecked())
        automationWriteAction->setChecked(false);
    if (automationWriteAction != nullptr)
        automationWriteAction->setEnabled(writeAutomationTarget);
    if (automationWriteModeControl != nullptr)
        automationWriteModeControl->setEnabled(writeAutomationTarget);
    arrangementEditor->setAutomationView(automationViewAction != nullptr &&
                                          automationViewAction->isChecked(),
                                          target, minimum, maximum, defaultValue, pluginUri,
                                          static_cast<AutomationPoint::Curve>(
                                              std::clamp(automationCurveControl->currentData().toInt(), 0, 4)),
                                          automationLaneParameterControl->currentText());
}

void MainWindow::addAutomationPoint()
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
        return;
    const int parameter = automationParameterControl->currentData().toInt();
    const QString pluginUri = automationParameterControl->currentData(Qt::UserRole + 4).toString();
    const QVector<AutomationPoint>* currentPoints = automationCurve(
        static_cast<const InstrumentTrack&>(*track), parameter, pluginUri);
    if (currentPoints == nullptr)
        return;
    const bool normalizedPercent = parameter == 0 || parameter == 1;
    const double valueScale = normalizedPercent ? 100.0 : 1.0;
    const QVector<AutomationPoint>& points = *currentPoints;
    const QVector<AutomationPoint> before = points;
    QVector<AutomationPoint> after = before;
    const auto curve = static_cast<AutomationPoint::Curve>(
        std::clamp(automationCurveControl->currentData().toInt(), 0, 4));
    const AutomationPoint point{automationBeatControl->value(),
                                static_cast<float>(automationValueControl->value() / valueScale), curve};
    auto position = std::lower_bound(after.begin(), after.end(), point.beat,
        [](const AutomationPoint& current, double beat) { return current.beat < beat; });
    if (position != after.end() && std::abs(position->beat - point.beat) < 1.0e-9)
    {
        if (std::abs(position->value - point.value) < 1.0e-6f)
        {
            if (position->curveToNext == point.curveToNext)
                return;
        }
        position->value = point.value;
        position->curveToNext = point.curveToNext;
    }
    else
    {
        if (after.size() >= 10000)
        {
            statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
            return;
        }
        after.insert(position, point);
    }
    undoStack.push(new AutomationEditCommand(this, track->id, parameter, before, after, pluginUri));
}

void MainWindow::removeAutomationPoint()
{
    InstrumentTrack* track = selectedTrack();
    const QListWidgetItem* item = automationPointsList->currentItem();
    if (track == nullptr || item == nullptr)
        return;
    const int parameter = automationParameterControl->currentData().toInt();
    const QString pluginUri = automationParameterControl->currentData(Qt::UserRole + 4).toString();
    const QVector<AutomationPoint>* currentPoints = automationCurve(
        static_cast<const InstrumentTrack&>(*track), parameter, pluginUri);
    if (currentPoints == nullptr)
        return;
    const QVector<AutomationPoint> before = *currentPoints;
    QVector<AutomationPoint> after = before;
    const double beat = item->data(Qt::UserRole).toDouble();
    after.erase(std::remove_if(after.begin(), after.end(), [beat](const AutomationPoint& point)
    {
        return std::abs(point.beat - beat) < 1.0e-9;
    }), after.end());
    if (after.size() == before.size())
        return;
    undoStack.push(new AutomationEditCommand(this, track->id, parameter, before, after, pluginUri));
}

void MainWindow::setSelectedAutomationCurve(int curveValue)
{
    const QListWidgetItem* item = automationPointsList->currentItem();
    InstrumentTrack* track = selectedTrack();
    if (item == nullptr || track == nullptr)
    {
        refreshArrangementAutomationView();
        return;
    }
    const int parameter = automationParameterControl->currentData().toInt();
    const QString pluginUri = automationParameterControl->currentData(Qt::UserRole + 4).toString();
    const QVector<AutomationPoint>* currentPoints = automationCurve(
        static_cast<const InstrumentTrack&>(*track), parameter, pluginUri);
    if (currentPoints == nullptr)
        return;
    const double beat = item->data(Qt::UserRole).toDouble();
    const auto point = std::lower_bound(currentPoints->cbegin(), currentPoints->cend(), beat,
        [](const AutomationPoint& candidate, double targetBeat) { return candidate.beat < targetBeat; });
    if (point == currentPoints->cend() || std::abs(point->beat - beat) > 1.0e-9)
        return;
    const auto curve = static_cast<AutomationPoint::Curve>(std::clamp(curveValue, 0, 4));
    if (point->curveToNext == curve)
        return;
    const QVector<AutomationPoint> before = *currentPoints;
    QVector<AutomationPoint> after = before;
    const qsizetype index = point - currentPoints->cbegin();
    after[index].curveToNext = curve;
    undoStack.push(new AutomationEditCommand(this, track->id, parameter, before, after, pluginUri));
}

void MainWindow::beginAutomationWrite(quint64 trackId, int parameter, float value,
                                     const QString& pluginUri, qsizetype effectIndex,
                                     qsizetype parameterIndex, float minimum, float maximum,
                                     const QString& parameterName)
{
    if (automationWriteSession.active || automationWriteAction == nullptr ||
        !automationWriteAction->isChecked() || !audioEngine.isPlaying() ||
        automationLaneParameterControl->currentData().toInt() != parameter)
        return;
    InstrumentTrack* track = project.findTrack(trackId);
    const double beat = audioEngine.positionBeats();
    if (track == nullptr || beat < 0.0)
        return;
    QVector<AutomationPoint>* points = automationCurve(*track, parameter, pluginUri);
    if (points == nullptr)
        return;
    if (parameter == 0)
    {
        minimum = 0.0f;
        maximum = 1.0f;
    }
    else if (parameter == 1)
    {
        minimum = -1.0f;
        maximum = 1.0f;
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
        return;

    automationWriteSession.active = true;
    automationWriteSession.changed = false;
    automationWriteSession.trackId = trackId;
    automationWriteSession.parameter = parameter;
    automationWriteSession.pluginUri = pluginUri;
    automationWriteSession.effectIndex = effectIndex;
    automationWriteSession.parameterIndex = parameterIndex;
    automationWriteSession.minimum = minimum;
    automationWriteSession.maximum = maximum;
    automationWriteSession.parameterName = parameterName;
    automationWriteSession.initialValue = value;
    automationWriteSession.baseValue = parameter == 0 ? track->gain :
        parameter == 1 ? track->pan : value;
    automationWriteSession.lastValue = value;
    automationWriteSession.lastSampleBeat = beat;
    automationWriteSession.lastPointBeat = beat;
    automationWriteSession.before = *points;
    const bool trimMode = automationWriteModeControl->currentData().toInt() == 2;
    const float initialAutomationValue = automationValueAt(
        *points, beat, automationWriteSession.baseValue);
    const float initialWriteValue = trimMode ? initialAutomationValue : value;
    if (!upsertAutomationPoint(*points, beat, initialWriteValue))
    {
        automationWriteSession = {};
        statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
        return;
    }
    if (pluginUri.isEmpty())
        audioEngine.setLiveTrackMixOverride(trackId, parameter, initialWriteValue);
    else
        audioEngine.setLivePluginParameterOverride(trackId, effectIndex, parameterIndex, initialWriteValue);
}

void MainWindow::recordAutomationValue(quint64 trackId, int parameter, float value)
{
    if (!automationWriteSession.active || automationWriteSession.trackId != trackId ||
        automationWriteSession.parameter != parameter)
        return;
    const double beat = audioEngine.positionBeats();
    if (beat < 0.0)
        return;
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    QVector<AutomationPoint>* points = automationCurve(*track, parameter,
                                                        automationWriteSession.pluginUri);
    if (points == nullptr)
        return;

    const bool trimMode = automationWriteModeControl->currentData().toInt() == 2;
    const float underlyingValue = automationValueAt(
        automationWriteSession.before, beat, automationWriteSession.baseValue);
    const float writeValue = trimMode
        ? std::clamp(underlyingValue + value - automationWriteSession.initialValue,
                     automationWriteSession.minimum, automationWriteSession.maximum)
        : value;

    if (beat + 1.0e-6 < automationWriteSession.lastSampleBeat)
    {
        automationWriteSession.lastPointBeat = -1.0;
        automationWriteSession.lastSampleBeat = -1.0;
    }
    double writeBeat = beat;
    const bool coalesce = automationWriteSession.lastPointBeat >= 0.0 &&
                          beat >= automationWriteSession.lastPointBeat &&
                          beat - automationWriteSession.lastPointBeat < automationWritePointSpacingBeats;
    if (coalesce)
        writeBeat = automationWriteSession.lastPointBeat;

    if (!upsertAutomationPoint(*points, writeBeat, writeValue))
        statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
    else if (!coalesce)
        automationWriteSession.lastPointBeat = writeBeat;
    automationWriteSession.lastSampleBeat = beat;
    automationWriteSession.changed = automationWriteSession.changed ||
                                     (trimMode ? std::abs(writeValue - underlyingValue)
                                      : std::abs(value - automationWriteSession.initialValue)) > 1.0e-6f;
    automationWriteSession.lastValue = value;
    if (automationWriteSession.pluginUri.isEmpty())
        audioEngine.setLiveTrackMixOverride(trackId, parameter, writeValue);
    else
        audioEngine.setLivePluginParameterOverride(trackId, automationWriteSession.effectIndex,
                                                   automationWriteSession.parameterIndex, writeValue);
}

void MainWindow::finishAutomationWrite()
{
    if (!automationWriteSession.active)
        return;
    automationWriteIdleTimer->stop();
    AutomationWriteSession session = automationWriteSession;
    automationWriteSession = {};
    if (session.pluginUri.isEmpty())
        audioEngine.clearLiveTrackMixOverride(session.trackId, session.parameter);
    else
        audioEngine.clearLivePluginParameterOverride(session.trackId, session.effectIndex,
                                                     session.parameterIndex);

    InstrumentTrack* track = project.findTrack(session.trackId);
    if (track == nullptr)
        return;
    QVector<AutomationPoint>* current = automationCurve(*track, session.parameter, session.pluginUri);
    if (current == nullptr)
        return;
    if (!session.changed || sameAutomationPoints(session.before, *current))
    {
        *current = session.before;
        audioEngine.setProject(project);
        return;
    }

    const QVector<AutomationPoint> after = *current;
    undoStack.push(new AutomationEditCommand(this, session.trackId, session.parameter,
                                             session.before, after, session.pluginUri));
    const QString parameterName = session.parameterName.isEmpty()
        ? session.parameter == 0 ? tr("volume") : tr("pan") : session.parameterName;
    statusBar()->showMessage(tr("Recorded %1 automation for %2")
                                 .arg(parameterName, track->name), 2500);
}

void MainWindow::applyAutomationHistory(quint64 trackId, int parameter, const QString& pluginUri,
                                        const QVector<AutomationPoint>& points)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    QVector<AutomationPoint>* curve = automationCurve(*track, parameter, pluginUri);
    if (curve == nullptr)
        return;
    *curve = points;
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    refreshAutomationEditor();
    refreshDirtyState();
}

void MainWindow::addTimelineMarker(double beat)
{
    if (project.markers.size() >= 1000)
    {
        statusBar()->showMessage(tr("This project has reached the 1,000-marker limit"), 3500);
        return;
    }
    beat = std::clamp(beat, 0.0, std::max(0.0, project.songLengthBeats() - 1.0 / 64.0));
    for (const TimelineMarker& marker : project.markers)
    {
        if (std::abs(marker.beat - beat) < 1.0e-6)
        {
            statusBar()->showMessage(tr("A marker already exists at that beat"), 2500);
            return;
        }
    }

    bool accepted = false;
    const QString defaultName = tr("Marker %1").arg(project.markers.size() + 1);
    const QString name = QInputDialog::getText(this, tr("Add arrangement marker"), tr("Name"),
                                                QLineEdit::Normal, defaultName, &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;

    const QVector<TimelineMarker> before = project.markers;
    QVector<TimelineMarker> after = before;
    after.push_back({beat, name.left(80)});
    std::stable_sort(after.begin(), after.end(), [](const TimelineMarker& left, const TimelineMarker& right)
    {
        return left.beat < right.beat;
    });
    undoStack.push(new MarkerEditCommand(this, before, after));
    statusBar()->showMessage(tr("Added marker %1").arg(name), 2000);
}

void MainWindow::renameTimelineMarker(double beat)
{
    int markerIndex = -1;
    for (int index = 0; index < project.markers.size(); ++index)
    {
        if (std::abs(project.markers[index].beat - beat) < 1.0e-6)
        {
            markerIndex = index;
            break;
        }
    }
    if (markerIndex < 0)
        return;

    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Rename arrangement marker"), tr("Name"),
                                                QLineEdit::Normal, project.markers[markerIndex].name,
                                                &accepted).trimmed();
    if (!accepted || name.isEmpty() || name == project.markers[markerIndex].name)
        return;

    const QVector<TimelineMarker> before = project.markers;
    QVector<TimelineMarker> after = before;
    after[markerIndex].name = name.left(80);
    undoStack.push(new MarkerEditCommand(this, before, after));
    statusBar()->showMessage(tr("Renamed arrangement marker"), 2000);
}

void MainWindow::removeTimelineMarker(double beat)
{
    int markerIndex = -1;
    for (int index = 0; index < project.markers.size(); ++index)
    {
        if (std::abs(project.markers[index].beat - beat) < 1.0e-6)
        {
            markerIndex = index;
            break;
        }
    }
    if (markerIndex < 0)
        return;

    const QVector<TimelineMarker> before = project.markers;
    QVector<TimelineMarker> after = before;
    const QString name = after[markerIndex].name;
    after.removeAt(markerIndex);
    undoStack.push(new MarkerEditCommand(this, before, after));
    statusBar()->showMessage(tr("Removed marker %1").arg(name), 2000);
}

void MainWindow::seekToTimelineMarker(double beat)
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
    {
        statusBar()->showMessage(tr("Stop recording before seeking to an arrangement marker"), 3000);
        return;
    }
    if (!audioEngine.isAudioAvailable() && !audioEngine.initialize())
    {
        statusBar()->showMessage(audioEngine.errorMessage(), 4000);
        return;
    }
    audioEngine.setProject(project);
    if (!audioEngine.isPlaying())
        audioEngine.startPlayback();
    audioEngine.seekToBeat(beat);
    playAction->setChecked(true);
    statusBar()->showMessage(tr("Playing from beat %1").arg(beat + 1.0, 0, 'f', 2), 2000);
}

void MainWindow::applyMarkerHistory(const QVector<TimelineMarker>& markers)
{
    project.markers = markers;
    project.normalize();
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    refreshDirtyState();
}

void MainWindow::setTimelineTempo(double beat)
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
    {
        statusBar()->showMessage(tr("Stop recording before editing the tempo map"), 3000);
        return;
    }

    beat = std::clamp(beat, 0.0, 16384.0);
    double targetBeat = std::round(beat * 4.0) / 4.0;
    for (const TempoChange& change : project.tempoChanges)
    {
        if (std::abs(change.beat - beat) < 1.0e-6)
        {
            targetBeat = change.beat;
            break;
        }
    }
    targetBeat = std::clamp(targetBeat, 0.0, 16384.0);
    const double currentTempo = project.tempoAtBeat(targetBeat);
    bool accepted = false;
    const double bpm = QInputDialog::getDouble(this, tr("Set arrangement tempo"),
        tr("Tempo at beat %1").arg(targetBeat + 1.0, 0, 'f', 2), currentTempo,
        20.0, 300.0, 1, &accepted);
    if (!accepted || std::abs(bpm - currentTempo) < 1.0e-9)
        return;

    const double beforeBaseTempo = project.tempoBpm;
    const QVector<TempoChange> before = project.tempoChanges;
    double afterBaseTempo = beforeBaseTempo;
    QVector<TempoChange> after = before;
    if (targetBeat < 1.0e-9)
    {
        afterBaseTempo = bpm;
    }
    else
    {
        auto position = std::lower_bound(after.begin(), after.end(), targetBeat,
            [](const TempoChange& change, double value) { return change.beat < value; });
        if (position != after.end() && std::abs(position->beat - targetBeat) < 1.0e-6)
            position->bpm = bpm;
        else
        {
            if (after.size() >= 1000)
            {
                statusBar()->showMessage(tr("This project has reached the 1,000-tempo-change limit"), 3500);
                return;
            }
            after.insert(position, {targetBeat, bpm});
        }
    }
    undoStack.push(new TempoMapEditCommand(this, beforeBaseTempo, before,
                                           afterBaseTempo, after));
    statusBar()->showMessage(tr("Tempo set to %1 BPM at beat %2")
                                 .arg(bpm, 0, 'f', 1).arg(targetBeat + 1.0, 0, 'f', 2), 2500);
}

void MainWindow::removeTimelineTempo(double beat)
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
    {
        statusBar()->showMessage(tr("Stop recording before editing the tempo map"), 3000);
        return;
    }
    for (qsizetype index = 0; index < project.tempoChanges.size(); ++index)
    {
        if (std::abs(project.tempoChanges[index].beat - beat) >= 1.0e-6)
            continue;
        const double beforeBaseTempo = project.tempoBpm;
        const QVector<TempoChange> before = project.tempoChanges;
        QVector<TempoChange> after = before;
        after.removeAt(index);
        undoStack.push(new TempoMapEditCommand(this, beforeBaseTempo, before,
                                               beforeBaseTempo, after));
        statusBar()->showMessage(tr("Removed tempo change; tempo returns to %1 BPM")
                                     .arg(project.tempoAtBeat(beat), 0, 'f', 1), 2500);
        return;
    }
}

void MainWindow::applyTempoHistory(double baseTempo, const QVector<TempoChange>& changes)
{
    project.tempoBpm = baseTempo;
    project.tempoChanges = changes;
    project.normalize();
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    {
        const QSignalBlocker blocker(tempoControl);
        tempoControl->setValue(project.tempoBpm);
    }
    refreshDirtyState();
}

bool MainWindow::maybeSave()
{
    finishAutomationWrite();
    if (audioRecorder || recordingPending)
        finishAudioRecording(true);
    if (midiRecordingActive || midiRecordingPending)
        finishMidiRecording(true);
    if (!isWindowModified())
    {
        QFile::remove(projectPath.isEmpty() ? unsavedSessionAutosavePath()
                                            : projectAutosavePath(projectPath));
        return true;
    }

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, tr("Unsaved project"), tr("Save changes to %1?").arg(project.title),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Save)
    {
        if (projectPath.isEmpty())
        {
            QString path = QFileDialog::getSaveFileName(this, tr("Save project"), QString(),
                                                         tr("Awesome Studio project (*.aws)"));
            if (path.isEmpty()) return false;
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".aws");
            return saveToPath(path);
        }
        return saveToPath(projectPath);
    }
    if (choice == QMessageBox::Discard)
    {
        if (!projectPath.isEmpty())
            QFile::remove(projectAutosavePath(projectPath));
        QFile::remove(unsavedSessionAutosavePath());
        return true;
    }
    return false;
}

void MainWindow::setProject(const ProjectData& incoming, const QString& path)
{
    finishAutomationWrite();
    audioEngine.stopPlayback();
    project = incoming;
    project.normalize();
    projectPath = path;
    settingsDirty = false;
    undoStack.clear();
    undoStack.setClean();
    selectedTrackId = project.tracks.isEmpty() ? 0 : project.tracks.first().id;
    selectedClipId = project.tracks.isEmpty() || project.tracks.first().clips.isEmpty()
        ? 0 : project.tracks.first().clips.first().id;
    refreshProjectUi();
    playAction->setChecked(false);
    midiEditor->setPlayheadBeat(0.0);
    arrangementEditor->setPlayheadBeat(0.0);
    updateProjectTitle();
    int offlineSources = 0;
    for (const InstrumentTrack& track : project.tracks)
    {
        for (const MidiClip& clip : track.clips)
            if (clip.isAudioRegion() && clip.audioSource == nullptr)
                ++offlineSources;
        for (const DrumPadSample& sample : track.drumSamples)
        {
            if (!sample.path.isEmpty() && sample.source == nullptr) ++offlineSources;
            if (!sample.veryLowVelocityPath.isEmpty() && sample.veryLowVelocitySource == nullptr)
                ++offlineSources;
            if (!sample.lowVelocityPath.isEmpty() && sample.lowVelocitySource == nullptr)
                ++offlineSources;
            if (!sample.highVelocityPath.isEmpty() && sample.highVelocitySource == nullptr)
                ++offlineSources;
            if (!sample.veryHighVelocityPath.isEmpty() && sample.veryHighVelocitySource == nullptr)
                ++offlineSources;
        }
    }
    if (offlineSources > 0)
        statusBar()->showMessage(tr("Project opened with %1 offline audio source reference(s); restore media or reduce loaded audio.")
                                     .arg(offlineSources), 6000);
}

bool MainWindow::saveToPath(const QString& path)
{
    finishAutomationWrite();
    if (audioRecorder || recordingPending)
        finishAudioRecording(true);
    if (midiRecordingActive || midiRecordingPending)
        finishMidiRecording(true);
    audioEngine.capturePluginStates(project);
    QString savePath = path;
    if (QFileInfo(savePath).suffix().isEmpty())
        savePath += QStringLiteral(".aws");
    QString stagedBackupPath;
    QString backupError;
    const bool backupStaged = stageProjectBackup(savePath, stagedBackupPath, backupError);
    const QString previousTitle = project.title;
    project.title = QFileInfo(savePath).completeBaseName();
    QString error;
    if (!ProjectFile::save(savePath, project, &error))
    {
        if (!stagedBackupPath.isEmpty())
            QFile::remove(stagedBackupPath);
        project.title = previousTitle;
        QMessageBox::critical(this, tr("Could not save project"), error);
        return false;
    }

    if (backupStaged && !stagedBackupPath.isEmpty())
        backupError = rotateProjectBackups(savePath, stagedBackupPath);

    projectPath = savePath;
    QFile::remove(projectAutosavePath(savePath));
    QFile::remove(unsavedSessionAutosavePath());
    settingsDirty = false;
    undoStack.setClean();
    audioEngine.setProject(project);
    updateProjectTitle();
    if (!backupError.isEmpty())
        statusBar()->showMessage(tr("Saved %1, but backup update failed: %2")
                                     .arg(QFileInfo(savePath).fileName(), backupError), 7000);
    else
        statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(savePath).fileName()), 2500);
    return true;
}

void MainWindow::savePortableProjectCopy()
{
    QString savePath = QFileDialog::getSaveFileName(this, tr("Save portable project copy"), projectPath,
                                                    tr("Awesome Studio project (*.aws)"));
    if (savePath.isEmpty())
        return;
    if (QFileInfo(savePath).suffix().isEmpty())
        savePath += QStringLiteral(".aws");
    if (!projectPath.isEmpty() &&
        QDir::cleanPath(QFileInfo(savePath).absoluteFilePath()) ==
            QDir::cleanPath(QFileInfo(projectPath).absoluteFilePath()))
    {
        QMessageBox::warning(this, tr("Choose a different project path"),
                             tr("A portable copy needs a different file name from the open project."));
        return;
    }

    finishAutomationWrite();
    if (audioRecorder || recordingPending)
        finishAudioRecording(true);
    if (midiRecordingActive || midiRecordingPending)
        finishMidiRecording(true);

    audioEngine.capturePluginStates(project);
    ProjectData portable = project;
    portable.normalize();
    const QFileInfo targetInfo(savePath);
    const QString outputDirectoryPath = targetInfo.absolutePath();
    const QString baseName = targetInfo.completeBaseName().isEmpty()
        ? QStringLiteral("project") : targetInfo.completeBaseName();
    const QString mediaDirectoryName = baseName + QStringLiteral("_media");
    const QString finalMediaDirectory = QDir(outputDirectoryPath).filePath(mediaDirectoryName);
    if (QFileInfo::exists(finalMediaDirectory))
    {
        QMessageBox::warning(this, tr("Media folder already exists"),
                             tr("The portable copy cannot replace the existing folder %1.")
                                 .arg(finalMediaDirectory));
        return;
    }

    QTemporaryDir staging(QDir(outputDirectoryPath).filePath(
        QStringLiteral(".awesome-studio-media-XXXXXX")));
    if (!staging.isValid())
    {
        QMessageBox::critical(this, tr("Could not prepare portable copy"), staging.errorString());
        return;
    }
    const QString stagedMediaDirectory = staging.filePath(mediaDirectoryName);
    if (!QDir().mkpath(stagedMediaDirectory))
    {
        QMessageBox::critical(this, tr("Could not prepare portable copy"),
                              tr("The temporary media folder could not be created."));
        return;
    }

    QHash<QString, QString> copiedPaths;
    int copiedAssetCount = 0;
    int unavailableAssetCount = 0;
    QString copyError;
    const auto collectPath = [&](QString& storedPath)
    {
        if (storedPath.isEmpty())
            return true;
        QString sourcePath = storedPath;
        if (QDir::isRelativePath(sourcePath))
        {
            const QString sourceBase = projectPath.isEmpty()
                ? QDir::currentPath() : QFileInfo(projectPath).absolutePath();
            sourcePath = QDir(sourceBase).absoluteFilePath(sourcePath);
        }
        const QFileInfo sourceInfo(sourcePath);
        if (!sourceInfo.exists() || !sourceInfo.isFile())
        {
            ++unavailableAssetCount;
            storedPath = sourceInfo.absoluteFilePath();
            return true;
        }
        QString cacheKey = sourceInfo.canonicalFilePath();
        if (cacheKey.isEmpty())
            cacheKey = sourceInfo.absoluteFilePath();
        const auto existing = copiedPaths.constFind(cacheKey);
        if (existing != copiedPaths.cend())
        {
            storedPath = *existing;
            return true;
        }

        const QString fileName = sourceInfo.fileName().isEmpty()
            ? QStringLiteral("audio.wav") : sourceInfo.fileName();
        const QString relativePath = QStringLiteral("%1/%2_%3")
            .arg(mediaDirectoryName)
            .arg(copiedAssetCount + 1, 4, 10, QLatin1Char('0'))
            .arg(fileName);
        const QString stagedPath = staging.filePath(relativePath);
        if (!QDir().mkpath(QFileInfo(stagedPath).absolutePath()) ||
            !QFile::copy(sourceInfo.absoluteFilePath(), stagedPath))
        {
            copyError = tr("Could not copy %1 into the portable media folder.")
                .arg(sourceInfo.fileName());
            return false;
        }
        copiedPaths.insert(cacheKey, relativePath);
        storedPath = relativePath;
        ++copiedAssetCount;
        return true;
    };

    bool collected = true;
    for (InstrumentTrack& track : portable.tracks)
    {
        for (MidiClip& clip : track.clips)
        {
            if (!collectPath(clip.audioSourcePath))
            {
                collected = false;
                break;
            }
        }
        if (!collected)
            break;
        for (DrumPadSample& sample : track.drumSamples)
        {
            if (!collectPath(sample.path) || !collectPath(sample.veryLowVelocityPath) ||
                !collectPath(sample.lowVelocityPath) || !collectPath(sample.highVelocityPath) ||
                !collectPath(sample.veryHighVelocityPath))
            {
                collected = false;
                break;
            }
        }
        if (!collected)
            break;
    }
    if (!collected)
    {
        QMessageBox::critical(this, tr("Could not collect project media"), copyError);
        return;
    }
    portable.title = baseName;

    bool mediaInstalled = false;
    if (copiedAssetCount > 0)
    {
        QDir outputDirectory(outputDirectoryPath);
        const QString stagedRelative = outputDirectory.relativeFilePath(stagedMediaDirectory);
        if (!outputDirectory.rename(stagedRelative, mediaDirectoryName))
        {
            QMessageBox::critical(this, tr("Could not install portable media"),
                                  tr("The media folder could not be moved into the destination."));
            return;
        }
        mediaInstalled = true;
    }

    QString saveError;
    if (!ProjectFile::save(savePath, portable, &saveError))
    {
        if (mediaInstalled)
            QDir(finalMediaDirectory).removeRecursively();
        QMessageBox::critical(this, tr("Could not save portable copy"), saveError);
        return;
    }

    const QString summary = unavailableAssetCount == 0
        ? tr("Saved portable copy with %1 media file(s): %2")
              .arg(copiedAssetCount).arg(savePath)
        : tr("Saved portable copy with %1 media file(s); %2 unavailable source(s) remain linked by path: %3")
              .arg(copiedAssetCount).arg(unavailableAssetCount).arg(savePath);
    statusBar()->showMessage(summary, 8000);
}

void MainWindow::offerUnsavedSessionRecovery()
{
    const QString path = unsavedSessionAutosavePath();
    if (!QFileInfo::exists(path))
        return;

    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, tr("Recover unsaved session"),
        tr("An autosaved unsaved session from %1 is available. Recover it?")
            .arg(QFileInfo(path).lastModified().toString(Qt::ISODate)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes)
    {
        QFile::remove(path);
        return;
    }

    ProjectData recovered;
    QString error;
    if (!ProjectFile::load(path, recovered, &error))
    {
        QFile::remove(path);
        QMessageBox::warning(this, tr("Autosave could not be recovered"), error);
        return;
    }
    setProject(recovered);
    settingsDirty = true;
    refreshDirtyState();
    statusBar()->showMessage(tr("Recovered unsaved session; save it to keep the project."), 7000);
}

void MainWindow::autosaveProject()
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending ||
        automationWriteSession.active)
        return;
    if (!isWindowModified())
    {
        QFile::remove(projectPath.isEmpty() ? unsavedSessionAutosavePath()
                                            : projectAutosavePath(projectPath));
        return;
    }

    QString error;
    const QString path = projectPath.isEmpty()
        ? unsavedSessionAutosavePath() : projectAutosavePath(projectPath);
    audioEngine.capturePluginStates(project);
    if (!ProjectFile::save(path, project, &error))
        statusBar()->showMessage(tr("Autosave failed: %1").arg(error), 6000);
}

void MainWindow::updateProjectTitle()
{
    const QString name = projectPath.isEmpty() ? project.title : QFileInfo(projectPath).completeBaseName();
    setWindowTitle(tr("Awesome Studio — %1[*]").arg(name));
    refreshDirtyState();
}

void MainWindow::refreshDirtyState()
{
    setWindowModified(settingsDirty || !undoStack.isClean());
}

void MainWindow::applyPitchBendRangeHistory(double semitones)
{
    project.pitchBendRangeSemitones = semitones;
    project.normalize();
    midiEditor->setPitchBendRange(project.pitchBendRangeSemitones);
    audioEngine.setProject(project);
    refreshDirtyState();
}

void MainWindow::applyNoteHistory(quint64 trackId, quint64 clipId, const QVector<MidiNote>& notes,
                                  double clipLengthBeats,
                                  const QVector<MidiPitchBend>* pitchBends)
{
    MidiClip* clip = project.findClip(trackId, clipId);
    if (clip == nullptr)
        return;
    clip->notes = notes;
    if (pitchBends != nullptr)
        clip->pitchBends = *pitchBends;
    if (selectedTrackId == trackId && selectedClipId == clipId)
    {
        if (clipLengthBeats >= 0.0)
        {
            clip->lengthBeats = std::clamp(clipLengthBeats, 0.25, 16384.0);
            midiEditor->setPatternLengthBeats(clip->lengthBeats);
            const QSignalBlocker lengthBlocker(clipLengthControl);
            clipLengthControl->setValue(clip->lengthBeats);
        }
        midiEditor->setNotes(notes);
        if (pitchBends != nullptr)
            midiEditor->setPitchBends(*pitchBends);
    }
    else if (clipLengthBeats >= 0.0)
    {
        clip->lengthBeats = std::clamp(clipLengthBeats, 0.25, 16384.0);
    }
    refreshPianoRollGhostNotes();
    const MidiClip* currentClip = selectedClip();
    stepSequencer->setPattern(currentClip == nullptr ? QVector<MidiNote>{} : currentClip->notes,
                              currentClip == nullptr ? project.patternLengthBeats() : currentClip->lengthBeats,
                              project.quarterNotesPerBar(), project.nextNoteId(), currentClip != nullptr,
                              currentClip == nullptr ? 16 : currentClip->stepsPerBar);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    audioEngine.setProject(project);
    refreshDirtyState();
}

void MainWindow::applyStepDivisionHistory(quint64 trackId, quint64 clipId, int requestedStepsPerBar)
{
    MidiClip* clip = project.findClip(trackId, clipId);
    if (clip == nullptr)
        return;
    clip->stepsPerBar = requestedStepsPerBar;
    project.normalize();
    const MidiClip* currentClip = selectedClip();
    stepSequencer->setPattern(currentClip == nullptr ? QVector<MidiNote>{} : currentClip->notes,
                              currentClip == nullptr ? project.patternLengthBeats() : currentClip->lengthBeats,
                              project.quarterNotesPerBar(), project.nextNoteId(), currentClip != nullptr,
                              currentClip == nullptr ? 16 : currentClip->stepsPerBar);
    audioEngine.setProject(project);
    refreshDirtyState();
}

void MainWindow::exportAudio()
{
    WavExportOptions& exportOptions = previousWavExportOptions();
    if (!chooseWavExportOptions(this, exportOptions))
        return;
    bool useRange = false;
    double startBeat = 0.0;
    double endBeat = 0.0;
    if (!chooseExportRange(this, project.songLengthBeats(), useRange, startBeat, endBeat))
        return;
    QString path = QFileDialog::getSaveFileName(this, tr("Export arrangement to WAV"),
                                                 project.title + QStringLiteral(".wav"),
                                                 tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".wav");

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool success = WavExporter::exportProject(project, path, &error,
        useRange ? startBeat : 0.0, useRange ? endBeat : -1.0, exportOptions);
    QApplication::restoreOverrideCursor();
    if (!success)
    {
        QMessageBox::critical(this, tr("Could not export WAV"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::exportStems()
{
    WavExportOptions& exportOptions = previousWavExportOptions();
    if (!chooseWavExportOptions(this, exportOptions))
        return;
    bool useRange = false;
    double startBeat = 0.0;
    double endBeat = 0.0;
    if (!chooseExportRange(this, project.songLengthBeats(), useRange, startBeat, endBeat))
        return;
    const QString defaultDirectory = projectPath.isEmpty()
        ? QDir::homePath() : QFileInfo(projectPath).absolutePath();
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Export track stems"), defaultDirectory);
    if (directory.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool success = WavExporter::exportStems(project, directory, &error,
        useRange ? startBeat : 0.0, useRange ? endBeat : -1.0, exportOptions);
    QApplication::restoreOverrideCursor();
    if (!success)
    {
        QMessageBox::critical(this, tr("Could not export track stems"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1 track stems to %2")
                                 .arg(project.tracks.size())
                                 .arg(QDir(directory).dirName()), 4000);
}

void MainWindow::importAudio()
{
    if (audioRecorder || recordingPending)
    {
        statusBar()->showMessage(tr("Finish the active recording before importing more audio."), 3000);
        return;
    }
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Import WAV audio"), QString(),
                                                       tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset = findProjectAudioAsset(path);
    QString error;
    if (asset == nullptr && !AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not import WAV audio"), error);
        return;
    }
    if (findProjectAudioAsset(path) == nullptr)
    {
        const quint64 currentBytes = projectAudioBytes();
        const quint64 assetBytes = static_cast<quint64>(asset->samples.size() + asset->waveformPeaks.size()) *
                                   sizeof(float);
        if (currentBytes > AudioFile::maximumProjectDecodedBytes ||
            assetBytes > AudioFile::maximumProjectDecodedBytes - currentBytes)
        {
            QMessageBox::warning(this, tr("Audio memory limit"),
                                 tr("This project can load up to 512 MiB of decoded audio. Remove another audio source first."));
            return;
        }
    }

    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = QFileInfo(path).completeBaseName();
    track.instrumentType = InstrumentType::Audio;
    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = QFileInfo(path).fileName();
    clip.audioSourcePath = asset->path;
    clip.audioSource = std::move(asset);
    const double durationSeconds = static_cast<double>(clip.audioSource->frameCount()) /
                                   clip.audioSource->sampleRate;
    const double durationBeats = project.beatAtSeconds(durationSeconds);
    clip.lengthBeats = std::clamp(durationBeats, 0.25, 16384.0);
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(arrangementEditor);
    statusBar()->showMessage(tr("Imported %1 into an audio track").arg(QFileInfo(path).fileName()), 3000);
}

void MainWindow::importDrumSample(int pad)
{
    if (audioRecorder || recordingPending)
    {
        statusBar()->showMessage(tr("Finish the active recording before loading more audio."), 3000);
        return;
    }
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()))
        return;

    const QString path = QFileDialog::getOpenFileName(this, tr("Load drum sample"), QString(),
                                                       tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset = findProjectAudioAsset(path);
    QString error;
    if (asset == nullptr && !AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not load drum sample"), error);
        return;
    }
    DrumPadSample& drumSample = track->drumSamples[static_cast<size_t>(pad)];
    const DrumPadSample previousSample = drumSample;
    drumSample.path = asset->path;
    drumSample.source = asset;
    if (projectAudioBytes() > AudioFile::maximumProjectDecodedBytes)
    {
        drumSample = previousSample;
        QMessageBox::warning(this, tr("Audio memory limit"),
                             tr("This project can load up to 512 MiB of decoded audio. Remove another audio source first."));
        return;
    }
    settingsDirty = true;
    const QString loadedFileName = QFileInfo(drumSample.path).fileName();
    refreshProjectUi();
    statusBar()->showMessage(tr("Drum sample set: %1")
                                 .arg(loadedFileName), 2500);
}

void MainWindow::clearDrumSample(int pad)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()))
        return;

    DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
    sample.path.clear();
    sample.source.reset();
    const bool hasOtherLayers = sample.hasSampleAssignment();
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(hasOtherLayers
        ? tr("Base drum layer cleared") : tr("Using synthesized sound for this drum pad"), 2000);
}

void MainWindow::importVeryLowVelocityDrumSample(int pad)
{
    importDrumVelocityLayer(pad, 0);
}

void MainWindow::clearVeryLowVelocityDrumSample(int pad)
{
    clearDrumVelocityLayer(pad, 0);
}

void MainWindow::importLowVelocityDrumSample(int pad)
{
    importDrumVelocityLayer(pad, 1);
}

void MainWindow::clearLowVelocityDrumSample(int pad)
{
    clearDrumVelocityLayer(pad, 1);
}

void MainWindow::importHighVelocityDrumSample(int pad)
{
    importDrumVelocityLayer(pad, 2);
}

void MainWindow::clearHighVelocityDrumSample(int pad)
{
    clearDrumVelocityLayer(pad, 2);
}

void MainWindow::importVeryHighVelocityDrumSample(int pad)
{
    importDrumVelocityLayer(pad, 3);
}

void MainWindow::clearVeryHighVelocityDrumSample(int pad)
{
    clearDrumVelocityLayer(pad, 3);
}

void MainWindow::importDrumVelocityLayer(int pad, int layer)
{
    if (audioRecorder || recordingPending)
    {
        statusBar()->showMessage(tr("Finish the active recording before loading more audio."), 3000);
        return;
    }
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()) || layer < 0 || layer > 3)
        return;

    DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
    QString* targetPath = nullptr;
    std::shared_ptr<const AudioAsset>* targetSource = nullptr;
    QString layerName;
    switch (layer)
    {
        case 0:
            targetPath = &sample.veryLowVelocityPath;
            targetSource = &sample.veryLowVelocitySource;
            layerName = tr("extra-low");
            break;
        case 1:
            targetPath = &sample.lowVelocityPath;
            targetSource = &sample.lowVelocitySource;
            layerName = tr("low");
            break;
        case 2:
            targetPath = &sample.highVelocityPath;
            targetSource = &sample.highVelocitySource;
            layerName = tr("high");
            break;
        case 3:
            targetPath = &sample.veryHighVelocityPath;
            targetSource = &sample.veryHighVelocitySource;
            layerName = tr("extra-high");
            break;
        default: return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
        tr("Load %1-velocity drum layer").arg(layerName), QString(), tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset = findProjectAudioAsset(path);
    QString error;
    if (asset == nullptr && !AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not load drum sample layer"), error);
        return;
    }
    const QString previousPath = *targetPath;
    const std::shared_ptr<const AudioAsset> previousSource = *targetSource;
    *targetPath = asset->path;
    *targetSource = asset;
    if (projectAudioBytes() > AudioFile::maximumProjectDecodedBytes)
    {
        *targetPath = previousPath;
        *targetSource = previousSource;
        QMessageBox::warning(this, tr("Audio memory limit"),
            tr("This project can load up to 512 MiB of decoded audio. Remove another audio source first."));
        return;
    }
    settingsDirty = true;
    const QString loadedFileName = QFileInfo(*targetPath).fileName();
    refreshProjectUi();
    statusBar()->showMessage(tr("%1-velocity layer set: %2").arg(layerName,
        loadedFileName), 2500);
}

void MainWindow::clearDrumVelocityLayer(int pad, int layer)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()) || layer < 0 || layer > 3)
        return;

    DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
    switch (layer)
    {
        case 0:
            sample.veryLowVelocityPath.clear();
            sample.veryLowVelocitySource.reset();
            break;
        case 1:
            sample.lowVelocityPath.clear();
            sample.lowVelocitySource.reset();
            break;
        case 2:
            sample.highVelocityPath.clear();
            sample.highVelocitySource.reset();
            break;
        case 3:
            sample.veryHighVelocityPath.clear();
            sample.veryHighVelocitySource.reset();
            break;
        default: return;
    }
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Velocity layer cleared"), 2000);
}

void MainWindow::resetSelectedDrumLayerSettings(int pad)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()))
        return;
    const int layer = drumLayerSettingsControls[static_cast<size_t>(pad)]->currentData().toInt();
    if (layer < 0 || layer >= 5)
        return;
    DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
    if (!sample.velocityLayerSettingsCustom[static_cast<size_t>(layer)])
        return;
    sample.velocityLayerSettings[static_cast<size_t>(layer)] = DrumLayerSettings{};
    sample.velocityLayerSettingsCustom[static_cast<size_t>(layer)] = false;
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Layer settings now use the shared pad controls"), 2000);
}

void MainWindow::setSelectedDrumSample(int pad, int parameter, double value)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()))
        return;
    DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
    if (parameter <= 2 && !sample.hasSampleAssignment())
        return;
    const bool editsLayerControls = parameter <= 2 || (parameter >= 4 && parameter <= 7);
    const int selectedLayer = drumLayerSettingsControls[static_cast<size_t>(pad)]
        ->currentData().toInt();
    const bool layerSpecific = selectedLayer >= 0 && selectedLayer < 5;
    if (editsLayerControls)
    {
        if (layerSpecific && !sample.hasLayerAssignment(selectedLayer))
            return;
        DrumLayerSettings settings = layerSpecific
            ? sample.layerSettings(selectedLayer) : sample.sharedLayerSettings();
        switch (parameter)
        {
            case 0: settings.tuningSemitones = static_cast<float>(std::clamp(value, -24.0, 24.0)); break;
            case 1: settings.startPositionPercent = static_cast<float>(std::clamp(value, 0.0, 99.0)); break;
            case 2: settings.loopEnabled = value >= 0.5; break;
            case 4: settings.attackSeconds = static_cast<float>(std::clamp(value, 0.0, 5.0)); break;
            case 5: settings.decaySeconds = static_cast<float>(std::clamp(value, 0.0, 5.0)); break;
            case 6: settings.sustainLevel = static_cast<float>(std::clamp(value, 0.0, 1.0)); break;
            case 7: settings.releaseSeconds = static_cast<float>(std::clamp(value, 0.0, 5.0)); break;
            default: return;
        }
        if (layerSpecific)
        {
            sample.setLayerSettings(selectedLayer, settings);
            resetDrumLayerSettingsButtons[static_cast<size_t>(pad)]->setEnabled(true);
        }
        else
            sample.setSharedLayerSettings(settings);
    }
    else
    {
        switch (parameter)
        {
            case 3:
                sample.highVelocityThreshold = std::clamp(static_cast<int>(std::lround(value)),
                    sample.lowVelocityThreshold + 1, sample.veryHighVelocityThreshold);
                break;
            case 8:
                sample.lowVelocityThreshold = std::clamp(static_cast<int>(std::lround(value)),
                    sample.veryLowVelocityThreshold, sample.highVelocityThreshold - 1);
                break;
            case 9:
                sample.veryLowVelocityThreshold = std::clamp(
                    static_cast<int>(std::lround(value)), 1, sample.lowVelocityThreshold);
                break;
            case 10:
                sample.veryHighVelocityThreshold = std::clamp(
                    static_cast<int>(std::lround(value)), sample.highVelocityThreshold, 127);
                break;
            default: return;
        }
    }
    const QSignalBlocker veryLowBlocker(veryLowVelocityThresholdControls[static_cast<size_t>(pad)]);
    const QSignalBlocker lowBlocker(lowVelocityThresholdControls[static_cast<size_t>(pad)]);
    const QSignalBlocker highBlocker(highVelocityThresholdControls[static_cast<size_t>(pad)]);
    const QSignalBlocker veryHighBlocker(veryHighVelocityThresholdControls[static_cast<size_t>(pad)]);
    veryLowVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(
        1, sample.lowVelocityThreshold);
    lowVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(
        sample.veryLowVelocityThreshold, sample.highVelocityThreshold - 1);
    highVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(
        sample.lowVelocityThreshold + 1, sample.veryHighVelocityThreshold);
    veryHighVelocityThresholdControls[static_cast<size_t>(pad)]->setRange(
        sample.highVelocityThreshold, 127);
    veryLowVelocityThresholdControls[static_cast<size_t>(pad)]->setValue(
        sample.veryLowVelocityThreshold);
    lowVelocityThresholdControls[static_cast<size_t>(pad)]->setValue(sample.lowVelocityThreshold);
    highVelocityThresholdControls[static_cast<size_t>(pad)]->setValue(sample.highVelocityThreshold);
    veryHighVelocityThresholdControls[static_cast<size_t>(pad)]->setValue(
        sample.veryHighVelocityThreshold);
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

quint64 MainWindow::projectAudioBytes() const
{
    std::unordered_set<const AudioAsset*> uniqueAssets;
    quint64 totalBytes = 0;
    const auto addAsset = [&uniqueAssets, &totalBytes](const std::shared_ptr<const AudioAsset>& asset)
    {
        if (asset == nullptr || !uniqueAssets.insert(asset.get()).second)
            return;
        const quint64 sampleCount = static_cast<quint64>(asset->samples.size()) +
                                    static_cast<quint64>(asset->waveformPeaks.size());
        if (totalBytes > AudioFile::maximumProjectDecodedBytes ||
            sampleCount > (AudioFile::maximumProjectDecodedBytes - totalBytes) / sizeof(float))
            totalBytes = AudioFile::maximumProjectDecodedBytes + 1;
        else
            totalBytes += sampleCount * sizeof(float);
    };

    for (const InstrumentTrack& track : project.tracks)
    {
        for (const MidiClip& clip : track.clips)
            addAsset(clip.audioSource);
        for (const DrumPadSample& sample : track.drumSamples)
        {
            addAsset(sample.source);
            addAsset(sample.veryLowVelocitySource);
            addAsset(sample.lowVelocitySource);
            addAsset(sample.highVelocitySource);
            addAsset(sample.veryHighVelocitySource);
        }
    }
    return totalBytes;
}

std::shared_ptr<const AudioAsset> MainWindow::findProjectAudioAsset(const QString& path) const
{
    const QFileInfo requestedInfo(path);
    const QString requestedPath = requestedInfo.canonicalFilePath().isEmpty()
        ? requestedInfo.absoluteFilePath() : requestedInfo.canonicalFilePath();
    const auto matchesPath = [&requestedPath](const std::shared_ptr<const AudioAsset>& asset)
    {
        if (asset == nullptr)
            return false;
        const QFileInfo assetInfo(asset->path);
        const QString assetPath = assetInfo.canonicalFilePath().isEmpty()
            ? assetInfo.absoluteFilePath() : assetInfo.canonicalFilePath();
        return assetPath == requestedPath;
    };

    for (const InstrumentTrack& track : project.tracks)
    {
        for (const MidiClip& clip : track.clips)
            if (matchesPath(clip.audioSource)) return clip.audioSource;
        for (const DrumPadSample& sample : track.drumSamples)
        {
            if (matchesPath(sample.source)) return sample.source;
            if (matchesPath(sample.veryLowVelocitySource)) return sample.veryLowVelocitySource;
            if (matchesPath(sample.lowVelocitySource)) return sample.lowVelocitySource;
            if (matchesPath(sample.highVelocitySource)) return sample.highVelocitySource;
            if (matchesPath(sample.veryHighVelocitySource)) return sample.veryHighVelocitySource;
        }
    }
    return {};
}

void MainWindow::importMidi()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import MIDI file"), QString(),
                                                       tr("Standard MIDI files (*.mid *.midi)"));
    if (path.isEmpty())
        return;

    ProjectData imported;
    QString error;
    if (!MidiFile::load(path, imported, &error))
    {
        QMessageBox::critical(this, tr("Could not import MIDI"), error);
        return;
    }
    setProject(imported);
    statusBar()->showMessage(tr("Imported %1 MIDI tracks from %2")
                                 .arg(project.tracks.size()).arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::exportMidi()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export arrangement to MIDI"),
                                                 project.title + QStringLiteral(".mid"),
                                                 tr("Standard MIDI files (*.mid *.midi)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".mid");

    QString error;
    if (!MidiFile::save(path, project, &error))
    {
        QMessageBox::critical(this, tr("Could not export MIDI"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::setAudioMonitoring(bool enabled)
{
    if (!enabled)
    {
        audioEngine.setAudioInputMonitoring(false);
        if (audioRecorder)
            audioRecorder->setMonitorCallback({});
        if (audioMonitor)
            finishAudioMonitoring();
        else
            refreshAudioInputs();
        return;
    }

    const auto reject = [this](const QString& message)
    {
        audioEngine.setAudioInputMonitoring(false);
        if (audioRecorder)
            audioRecorder->setMonitorCallback({});
        if (audioMonitor)
        {
            audioMonitor->setMonitorCallback({});
            QString ignoredError;
            audioMonitor->finish(&ignoredError);
            audioMonitor.reset();
        }
        const QSignalBlocker blocker(audioMonitorAction);
        audioMonitorAction->setChecked(false);
        refreshAudioInputs();
        statusBar()->showMessage(message, 5000);
    };

    if (recordingPending)
    {
        reject(tr("Wait for audio recording to start before enabling input monitoring."));
        return;
    }
    if (!audioEngine.isAudioAvailable() && !audioEngine.initialize())
    {
        reject(tr("Audio monitoring needs an available output device."));
        return;
    }
    audioDeviceLabel->setText(tr("Audio · %1").arg(audioEngine.deviceName()));

    const auto callback = [this](const float* samples, qsizetype frames, int channels, int sampleRate)
    {
        audioEngine.pushAudioInputMonitorFrames(samples, frames, channels, sampleRate);
    };
    if (audioRecorder)
    {
        audioRecorder->setMonitorCallback(callback);
    }
    else
    {
        const QByteArray selectedDeviceId = audioInputControl->currentData().toByteArray();
        QAudioDevice selectedDevice;
        for (const QAudioDevice& device : QMediaDevices::audioInputs())
        {
            if (device.id() == selectedDeviceId)
            {
                selectedDevice = device;
                break;
            }
        }
        if (selectedDevice.isNull())
        {
            reject(tr("Connect or select an audio input device before monitoring."));
            return;
        }

        audioMonitor = std::make_unique<AudioRecorder>();
        QString error;
        if (!audioMonitor->startMonitoring(selectedDevice, callback, &error))
        {
            audioMonitor.reset();
            reject(tr("Could not start audio monitoring: %1").arg(error));
            return;
        }
    }

    if (!audioEngine.setAudioInputMonitoring(true))
    {
        reject(tr("Audio monitoring needs an available output device."));
        return;
    }
    refreshAudioInputs();
    statusBar()->showMessage(audioRecorder
        ? tr("Monitoring live audio input through the master output")
        : tr("Monitoring audio input through the master output"), 3500);
}

void MainWindow::finishAudioMonitoring()
{
    if (!audioMonitor)
        return;
    audioEngine.setAudioInputMonitoring(false);
    audioMonitor->setMonitorCallback({});
    QString error;
    const bool finished = audioMonitor->finish(&error);
    audioMonitor.reset();
    {
        const QSignalBlocker blocker(audioMonitorAction);
        audioMonitorAction->setChecked(false);
    }
    refreshAudioInputs();
    if (!finished && !error.isEmpty())
        statusBar()->showMessage(tr("Audio monitoring stopped: %1").arg(error), 5000);
}

void MainWindow::refreshAudioInputs()
{
    if (audioInputControl == nullptr)
        return;

    const QByteArray previousId = audioInputControl->currentData().toByteArray();
    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    const QSignalBlocker blocker(audioInputControl);
    audioInputControl->clear();

    int selectedIndex = -1;
    int defaultIndex = -1;
    for (const QAudioDevice& device : devices)
    {
        audioInputControl->addItem(device.description(), device.id());
        const int index = audioInputControl->count() - 1;
        if (device.id() == previousId)
            selectedIndex = index;
        if (device.isDefault())
            defaultIndex = index;
    }

    if (devices.isEmpty())
    {
        audioInputControl->addItem(tr("No input devices"), QByteArray{});
        audioInputControl->setCurrentIndex(0);
    }
    else if (selectedIndex >= 0)
    {
        audioInputControl->setCurrentIndex(selectedIndex);
    }
    else
    {
        audioInputControl->setCurrentIndex(defaultIndex >= 0 ? defaultIndex : 0);
    }
    audioInputControl->setEnabled(!devices.isEmpty() && !audioRecorder && !audioMonitor && !recordingPending &&
                                  !midiRecordingActive && !midiRecordingPending);
    audioMonitorAction->setEnabled(audioEngine.isAudioAvailable() && !recordingPending &&
                                   (audioRecorder != nullptr || audioMonitor != nullptr || !devices.isEmpty()));
    recordAction->setEnabled(!midiRecordingActive && !midiRecordingPending &&
                             (audioRecorder || recordingPending || !devices.isEmpty()));
}

void MainWindow::refreshMidiInputs()
{
    if (midiInputControl == nullptr)
        return;

    const QString previousId = midiInputControl->currentData().toString();
    QString error;
    const QList<MidiInputPort> ports = MidiInput::availablePorts(&error);
    const QSignalBlocker blocker(midiInputControl);
    midiInputControl->clear();
    int selectedIndex = -1;
    for (const MidiInputPort& port : ports)
    {
        midiInputControl->addItem(port.name, port.id);
        if (port.id == previousId)
            selectedIndex = midiInputControl->count() - 1;
    }
    if (ports.isEmpty())
    {
        midiInputControl->addItem(MidiInput::isSupported()
                                      ? tr("No MIDI input devices")
                                      : tr("MIDI input unavailable in this build"),
                                  QString{});
        midiInputControl->setCurrentIndex(0);
    }
    else
    {
        midiInputControl->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    }
    midiInputControl->setEnabled(!ports.isEmpty() && !audioRecorder && !recordingPending &&
                                 !midiMonitorAction->isChecked() &&
                                 !midiRecordingActive && !midiRecordingPending &&
                                 !midiControllerListenerEnabled);
    midiRecordAction->setEnabled(!audioRecorder && !recordingPending &&
                                 (midiRecordingActive || midiRecordingPending || MidiInput::isSupported()));
    midiMonitorAction->setEnabled((!audioRecorder && !recordingPending) ||
                                  midiMonitorAction->isChecked());
}

void MainWindow::setMidiControllerListening(bool enabled)
{
    const auto rejectEnable = [this](const QString& message)
    {
        const QSignalBlocker blocker(midiControllerListenAction);
        midiControllerListenAction->setChecked(false);
        midiControllerListenerEnabled = false;
        pendingMidiControllerTarget = -1;
        statusBar()->showMessage(message, 5000);
        refreshMidiInputs();
    };

    if (!enabled)
    {
        midiControllerListenerEnabled = false;
        pendingMidiControllerTarget = -1;
        if (!midiMonitorAction->isChecked() && !midiRecordingActive && !midiRecordingPending)
        {
            if (midiInputPollTimer != nullptr)
                midiInputPollTimer->stop();
            if (midiInput != nullptr)
                midiInput->disconnect();
            activeMidiInputPortId.clear();
        }
        refreshMidiInputs();
        statusBar()->showMessage(tr("MIDI controller mappings stopped"), 1800);
        return;
    }

    if (audioRecorder || recordingPending)
    {
        rejectEnable(tr("Stop audio recording before enabling MIDI controller mappings."));
        return;
    }
    const QString portId = midiInputControl->currentData().toString();
    if (portId.isEmpty())
    {
        rejectEnable(MidiInput::isSupported()
            ? tr("No MIDI input devices are available. Connect a device and try again.")
            : tr("MIDI controller mappings require an ALSA enabled Linux build."));
        return;
    }
    if (midiInput == nullptr)
        midiInput = std::make_unique<MidiInput>();
    if (midiInput->isConnected() && activeMidiInputPortId != portId)
    {
        if (midiMonitorAction->isChecked() || midiRecordingActive || midiRecordingPending)
        {
            rejectEnable(tr("The active MIDI input is already being used by monitoring or recording."));
            return;
        }
        midiInput->disconnect();
    }
    if (!midiInput->isConnected())
    {
        QString error;
        if (!midiInput->connectTo(portId, &error))
        {
            rejectEnable(tr("Could not open MIDI input: %1").arg(error));
            return;
        }
    }
    activeMidiInputPortId = portId;
    midiControllerListenerEnabled = true;
    midiInputPollTimer->start();
    refreshMidiInputs();
    statusBar()->showMessage(tr("Listening for mapped MIDI controllers"), 2000);
}

void MainWindow::beginMidiControllerLearn(int target)
{
    const int targetCount = static_cast<int>(MidiControllerTarget::Tempo) + 1;
    if (target < 0 || target >= targetCount)
        return;
    if ((target == static_cast<int>(MidiControllerTarget::SelectedTrackLevel) ||
         target == static_cast<int>(MidiControllerTarget::SelectedTrackPan)) && selectedTrack() == nullptr)
    {
        statusBar()->showMessage(tr("Select a track before learning its MIDI control"), 3000);
        return;
    }
    if (midiInput == nullptr || !midiInput->isConnected())
    {
        if (midiControllerListenAction == nullptr)
            return;
        midiControllerListenAction->setChecked(true);
        if (midiInput == nullptr || !midiInput->isConnected())
            return;
    }
    else if (midiControllerListenAction != nullptr && !midiControllerListenAction->isChecked())
    {
        midiControllerListenAction->setChecked(true);
    }
    pendingMidiControllerTarget = target;
    statusBar()->showMessage(tr("Move a MIDI CC control to assign it to the selected target…"), 5000);
}

void MainWindow::clearMidiControllerMapping(int target)
{
    if (!midiControllerMappings.remove(target))
    {
        statusBar()->showMessage(tr("No MIDI controller mapping is assigned to that target"), 2200);
        return;
    }
    if (pendingMidiControllerTarget == target)
        pendingMidiControllerTarget = -1;
    saveMidiControllerMappings();
    statusBar()->showMessage(tr("MIDI controller mapping cleared"), 1800);
}

void MainWindow::loadMidiControllerMappings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("midiControllerMappings"));
    const int targetCount = static_cast<int>(MidiControllerTarget::Tempo) + 1;
    for (int target = 0; target < targetCount; ++target)
    {
        const QString prefix = QString::number(target);
        const int controller = settings.value(prefix + QStringLiteral("/controller"), -1).toInt();
        const int channel = settings.value(prefix + QStringLiteral("/channel"), -1).toInt();
        if (controller >= 0 && controller <= 127 && channel >= 0 && channel <= 15)
            midiControllerMappings.insert(target, {channel, controller});
    }
    settings.endGroup();
}

void MainWindow::saveMidiControllerMappings() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("midiControllerMappings"));
    const int targetCount = static_cast<int>(MidiControllerTarget::Tempo) + 1;
    for (int target = 0; target < targetCount; ++target)
    {
        const MidiControllerMapping mapping = midiControllerMappings.value(target);
        const QString prefix = QString::number(target);
        settings.setValue(prefix + QStringLiteral("/controller"), mapping.controller);
        settings.setValue(prefix + QStringLiteral("/channel"), mapping.channel);
    }
    settings.endGroup();
    settings.sync();
}

void MainWindow::handleMidiControlChange(int channel, int controller, int value)
{
    if (pendingMidiControllerTarget >= 0)
    {
        const int target = pendingMidiControllerTarget;
        pendingMidiControllerTarget = -1;
        midiControllerMappings.insert(target, {channel, controller});
        saveMidiControllerMappings();
        statusBar()->showMessage(tr("Mapped MIDI CC %1 on channel %2").arg(controller).arg(channel + 1), 3000);
        return;
    }

    for (auto it = midiControllerMappings.cbegin(); it != midiControllerMappings.cend(); ++it)
    {
        if (it->channel == channel && it->controller == controller)
            applyMidiControllerTarget(it.key(), value);
    }
}

void MainWindow::applyMidiControllerTarget(int target, int value)
{
    value = std::clamp(value, 0, 127);
    if (target == static_cast<int>(MidiControllerTarget::MasterLevel))
    {
        masterControl->setValue(static_cast<int>(std::lround(value * 100.0 / 127.0)));
        return;
    }
    if (target == static_cast<int>(MidiControllerTarget::Tempo))
    {
        tempoControl->setValue(20.0 + value * 280.0 / 127.0);
        return;
    }

    const InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
        return;
    const bool levelTarget = target == static_cast<int>(MidiControllerTarget::SelectedTrackLevel);
    QSlider* control = mixerDock->findChild<QSlider*>(
        QStringLiteral("mixerTrack%1_%2").arg(levelTarget ? QStringLiteral("Volume") : QStringLiteral("Pan"))
                                         .arg(track->id));
    if (control == nullptr)
        return;
    const int controlValue = levelTarget
        ? static_cast<int>(std::lround(value * 100.0 / 127.0))
        : value <= 64 ? static_cast<int>(std::lround((value - 64) * 100.0 / 64.0))
                      : static_cast<int>(std::lround((value - 64) * 100.0 / 63.0));
    control->setValue(controlValue);
}

void MainWindow::setMidiMonitorEnabled(bool enabled)
{
    const auto rejectEnable = [this](const QString& message)
    {
        const QSignalBlocker blocker(midiMonitorAction);
        midiMonitorAction->setChecked(false);
        statusBar()->showMessage(message, 5000);
        refreshMidiInputs();
    };

    if (!enabled)
    {
        audioEngine.stopLiveMidiNotes();
        if (!midiRecordingActive && !midiRecordingPending && !midiControllerListenerEnabled)
        {
            if (midiInputPollTimer != nullptr)
                midiInputPollTimer->stop();
            if (midiInput != nullptr)
                midiInput->disconnect();
            activeMidiInputPortId.clear();
        }
        refreshMidiInputs();
        statusBar()->showMessage(tr("MIDI monitoring stopped"), 1800);
        return;
    }

    if (audioRecorder || recordingPending)
    {
        rejectEnable(tr("Stop audio recording before enabling MIDI monitoring."));
        return;
    }
    const InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType == InstrumentType::Audio)
    {
        rejectEnable(tr("Select a synth or drum rack track before monitoring MIDI."));
        return;
    }
    const QString portId = midiInputControl->currentData().toString();
    if (portId.isEmpty())
    {
        rejectEnable(MidiInput::isSupported()
            ? tr("No MIDI input devices are available. Connect a device and try again.")
            : tr("MIDI monitoring requires an ALSA enabled Linux build."));
        return;
    }
    if (!audioEngine.isAudioAvailable() && !audioEngine.initialize())
    {
        rejectEnable(audioEngine.errorMessage());
        return;
    }
    if (midiInput == nullptr)
        midiInput = std::make_unique<MidiInput>();
    if (midiInput->isConnected() && activeMidiInputPortId != portId)
    {
        if (midiRecordingActive || midiRecordingPending)
        {
            rejectEnable(tr("The MIDI recording is using a different input device."));
            return;
        }
        midiInput->disconnect();
    }
    if (!midiInput->isConnected())
    {
        QString error;
        if (!midiInput->connectTo(portId, &error))
        {
            rejectEnable(tr("Could not open MIDI input: %1").arg(error));
            return;
        }
    }
    activeMidiInputPortId = portId;
    midiInputPollTimer->start();
    refreshMidiInputs();
    statusBar()->showMessage(tr("Monitoring MIDI on %1").arg(track->name), 2500);
}

void MainWindow::startAudioRecording()
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
        return;
    if (project.tracks.size() >= 256)
    {
        QMessageBox::warning(this, tr("Track limit"), tr("A project can contain up to 256 tracks."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    const QByteArray selectedDeviceId = audioInputControl == nullptr
        ? QByteArray{} : audioInputControl->currentData().toByteArray();
    QAudioDevice selectedDevice;
    for (const QAudioDevice& device : QMediaDevices::audioInputs())
    {
        if (device.id() == selectedDeviceId)
        {
            selectedDevice = device;
            break;
        }
    }
    if (selectedDevice.isNull())
    {
        QMessageBox::warning(this, tr("No audio input"),
                             tr("Connect or select an audio input device before recording."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    const QString defaultDirectory = projectPath.isEmpty()
        ? QDir::homePath() : QFileInfo(projectPath).absolutePath();
    QString path = QFileDialog::getSaveFileName(this, tr("Save recorded take"),
                                                 QDir(defaultDirectory).filePath(tr("Recording.wav")),
                                                 tr("WAV audio (*.wav)"));
    if (path.isEmpty())
    {
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".wav");
    else if (QFileInfo(path).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0)
    {
        QMessageBox::warning(this, tr("Unsupported recording format"),
                             tr("Recordings are saved as WAV files. Choose a .wav filename."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }
    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, tr("Replace recording?"),
                              tr("%1 already exists. Replace it when the take is saved?")
                                  .arg(QFileInfo(path).fileName()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }
    if (findProjectAudioAsset(path) != nullptr)
    {
        QMessageBox::warning(this, tr("Recording is already in use"),
                             tr("Choose a new WAV filename. This path is already used by audio in the current project."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    const bool playbackAvailable = audioEngine.isAudioAvailable() || audioEngine.initialize();
    const quint64 usedAudioBytes = projectAudioBytes();
    recordingDecodedByteLimit = usedAudioBytes >= AudioFile::maximumProjectDecodedBytes
        ? 0 : AudioFile::maximumProjectDecodedBytes - usedAudioBytes;
    if (recordingDecodedByteLimit == 0)
    {
        QMessageBox::warning(this, tr("Audio memory limit"),
                             tr("This project has reached its 512 MiB decoded audio limit. Remove an audio source before recording another take."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    if (audioMonitor)
        finishAudioMonitoring();
    pendingRecordingPath = path;
    pendingRecordingDeviceId = selectedDevice.id();
    pendingRecordingDeviceName = selectedDevice.description();
    recordingPending = true;
    recordingTransportActive = playbackAvailable;
    audioInputControl->setEnabled(false);
    tempoControl->setEnabled(false);
    timeSignatureControl->setEnabled(false);
    countInControl->setEnabled(false);
    loopAction->setEnabled(false);
    metronomeAction->setEnabled(false);
    midiInputControl->setEnabled(false);
    midiRecordAction->setEnabled(false);
    audioMonitorAction->setEnabled(false);
    if (playbackAvailable)
    {
        audioEngine.setProject(project);
        audioEngine.startPlayback();
        playAction->setChecked(true);
        statusBar()->showMessage(project.countInBars > 0
            ? tr("Waiting for count-in before recording to %1…").arg(QFileInfo(path).fileName())
            : tr("Starting recording to %1…").arg(QFileInfo(path).fileName()));
    }
    else
    {
        statusBar()->showMessage(tr("Audio output is unavailable; recording starts now without count-in or playback."), 5000);
        if (!startPendingAudioRecording(0.0))
            finishAudioRecording(false);
    }
}

bool MainWindow::startPendingAudioRecording(double playheadBeat)
{
    QAudioDevice selectedDevice;
    for (const QAudioDevice& device : QMediaDevices::audioInputs())
    {
        if (device.id() == pendingRecordingDeviceId)
        {
            selectedDevice = device;
            break;
        }
    }
    if (selectedDevice.isNull())
    {
        statusBar()->showMessage(tr("The selected audio input was disconnected."), 5000);
        return false;
    }

    audioRecorder = std::make_unique<AudioRecorder>();
    QString error;
    if (!audioRecorder->start(selectedDevice, pendingRecordingPath, recordingDecodedByteLimit, &error))
    {
        audioRecorder.reset();
        statusBar()->showMessage(tr("Could not start recording: %1").arg(error), 6000);
        return false;
    }

    recordingStartBeat = std::max(0.0, playheadBeat);
    recordingPending = false;
    pendingRecordingPath.clear();
    pendingRecordingDeviceId.clear();
    recordAction->setEnabled(true);
    audioMonitorAction->setEnabled(audioEngine.isAudioAvailable());
    statusBar()->showMessage(recordingTransportActive
        ? tr("Recording from %1…").arg(pendingRecordingDeviceName)
        : tr("Recording from %1 without playback…").arg(pendingRecordingDeviceName));
    pendingRecordingDeviceName.clear();
    return true;
}

void MainWindow::finishAudioRecording(bool keepTake)
{
    if (!audioRecorder && !recordingPending)
        return;

    audioEngine.setAudioInputMonitoring(false);
    if (audioRecorder)
        audioRecorder->setMonitorCallback({});
    {
        const QSignalBlocker blocker(audioMonitorAction);
        audioMonitorAction->setChecked(false);
        audioMonitorAction->setEnabled(false);
    }

    const bool wasWaitingForCountIn = recordingPending;
    const QString path = audioRecorder ? audioRecorder->path() : pendingRecordingPath;
    const bool reachedLimit = audioRecorder && audioRecorder->maximumReached();
    const QString captureIssue = audioRecorder ? audioRecorder->errorMessage() : QString{};
    bool finished = false;
    QString error;
    if (audioRecorder && keepTake)
        finished = audioRecorder->finish(&error);
    audioRecorder.reset();
    recordingPending = false;
    recordingTransportActive = false;
    pendingRecordingPath.clear();
    pendingRecordingDeviceId.clear();
    pendingRecordingDeviceName.clear();
    recordingDecodedByteLimit = 0;
    tempoControl->setEnabled(true);
    timeSignatureControl->setEnabled(true);
    countInControl->setEnabled(true);
    loopAction->setEnabled(true);
    metronomeAction->setEnabled(true);
    audioEngine.stopPlayback();
    playAction->setChecked(false);
    {
        const QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
    }
    refreshAudioInputs();
    refreshMidiInputs();

    if (!keepTake)
        return;
    if (wasWaitingForCountIn)
    {
        statusBar()->showMessage(tr("Recording stopped before the count-in finished; no take was created."), 3500);
        return;
    }
    if (!finished)
    {
        statusBar()->showMessage(tr("Recording was not saved: %1").arg(error), 6000);
        return;
    }

    std::shared_ptr<const AudioAsset> asset;
    if (!AudioFile::loadWav(path, asset, &error))
    {
        statusBar()->showMessage(tr("The take was written, but could not be added to the project: %1").arg(error), 6000);
        return;
    }

    const QString takeName = QFileInfo(path).completeBaseName();
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("Audio · %1").arg(takeName);
    track.instrumentType = InstrumentType::Audio;

    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = takeName;
    clip.startBeat = recordingStartBeat;
    const double durationSeconds = static_cast<double>(asset->frameCount()) / asset->sampleRate;
    const double endBeat = project.beatAtSeconds(project.secondsAtBeat(recordingStartBeat) +
                                                  durationSeconds);
    clip.lengthBeats = std::max(0.25, endBeat - recordingStartBeat);
    clip.audioSourcePath = asset->path;
    clip.audioSource = asset;
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    project.normalize();
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(arrangementEditor);
    refreshDirtyState();

    if (reachedLimit)
        statusBar()->showMessage(tr("Recording stopped at the project audio memory or WAV size limit; the take was added."), 6000);
    else if (!captureIssue.isEmpty())
        statusBar()->showMessage(tr("A partial take was added after an input error: %1").arg(captureIssue), 6000);
    else
        statusBar()->showMessage(tr("Recorded %1 to a new audio track").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::startMidiRecording()
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
        return;

    refreshMidiInputs();
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType == InstrumentType::Audio)
    {
        QMessageBox::warning(this, tr("Select an instrument track"),
                             tr("Select a synth or drum rack track before recording MIDI."));
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        return;
    }
    const QString portId = midiInputControl->currentData().toString();
    if (portId.isEmpty())
    {
        statusBar()->showMessage(MidiInput::isSupported()
            ? tr("No MIDI input devices are available. Connect a device and try again.")
            : tr("MIDI input requires an ALSA enabled Linux build."), 5000);
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        return;
    }

    const MidiClip* selected = selectedClip();
    quint64 targetClipId = selected != nullptr && !selected->isAudioRegion() &&
                           std::abs(selected->startBeat) < 1.0e-9 ? selected->id : 0;
    if (targetClipId == 0)
    {
        for (const MidiClip& clip : track->clips)
        {
            if (!clip.isAudioRegion() && std::abs(clip.startBeat) < 1.0e-9)
            {
                targetClipId = clip.id;
                break;
            }
        }
    }
    quint64 usedNotes = 0;
    if (targetClipId != 0)
    {
        const MidiClip* targetClip = project.findClip(track->id, targetClipId);
        if (targetClip != nullptr)
            usedNotes = static_cast<quint64>(targetClip->notes.size());
    }
    midiAvailableNoteSlots = usedNotes >= 100000 ? 0 : 100000 - usedNotes;
    if (midiAvailableNoteSlots == 0)
    {
        statusBar()->showMessage(tr("The target pattern already has the 100,000 note limit."), 4000);
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        return;
    }

    QString error;
    if ((!midiInput->isConnected() || activeMidiInputPortId != portId) &&
        !midiInput->connectTo(portId, &error))
    {
        statusBar()->showMessage(tr("Could not open MIDI input: %1").arg(error), 6000);
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        refreshMidiInputs();
        return;
    }
    activeMidiInputPortId = portId;

    midiRecordingTrackId = track->id;
    midiRecordingClipId = targetClipId;
    midiLastBeat = 0.0;
    midiOpenNoteCount = 0;
    openMidiNotes.clear();
    recordedMidiNotes.clear();
    recordedMidiPitchBends.clear();
    const bool playbackAvailable = audioEngine.isAudioAvailable() || audioEngine.initialize();
    midiTransportActive = playbackAvailable;
    midiRecordingPending = playbackAvailable;
    midiRecordingActive = false;
    tempoControl->setEnabled(false);
    timeSignatureControl->setEnabled(false);
    countInControl->setEnabled(false);
    loopAction->setEnabled(false);
    metronomeAction->setEnabled(false);
    audioInputControl->setEnabled(false);
    midiInputControl->setEnabled(false);
    recordAction->setEnabled(false);
    midiInputPollTimer->start();

    if (playbackAvailable)
    {
        audioEngine.setProject(project);
        audioEngine.startPlayback();
        playAction->setChecked(true);
        statusBar()->showMessage(project.countInBars > 0
            ? tr("Waiting for count-in before MIDI recording…")
            : tr("Starting MIDI recording…"));
    }
    else
    {
        statusBar()->showMessage(tr("Audio output is unavailable; MIDI recording starts now without count-in or playback."), 5000);
        if (!startPendingMidiRecording(0.0))
            finishMidiRecording(false);
    }
}

bool MainWindow::startPendingMidiRecording(double playheadBeat)
{
    if (midiInput == nullptr || !midiInput->isConnected())
    {
        statusBar()->showMessage(tr("The MIDI input disconnected before recording could start."), 5000);
        return false;
    }
    midiRecordingPending = false;
    midiRecordingActive = true;
    midiRecordStartBeat = std::max(0.0, playheadBeat);
    midiLastBeat = midiRecordStartBeat;
    midiRecordClock.start();
    statusBar()->showMessage(midiTransportActive
        ? tr("Recording MIDI into the bar 1 pattern…")
        : tr("Recording MIDI without playback…"));
    return true;
}

void MainWindow::pollMidiInput()
{
    if (midiInput == nullptr || !midiInput->isConnected())
        return;
    const QVector<MidiInputEvent> events = midiInput->readAvailable();
    if (events.isEmpty())
        return;

    for (const MidiInputEvent& event : events)
    {
        if (event.controlChange)
        {
            handleMidiControlChange(event.channel, event.controller, event.value);
            continue;
        }

        if (event.pitchBend)
        {
            if (midiMonitorAction->isChecked())
            {
                const InstrumentTrack* track = selectedTrack();
                audioEngine.triggerLiveMidiPitchBend(event.channel, event.value, track);
            }
            if (midiRecordingActive && recordedMidiPitchBends.size() < 100000)
            {
                recordedMidiPitchBends.push_back({event.channel, event.value,
                                                  currentMidiRecordingBeat()});
            }
            continue;
        }

        if (midiMonitorAction->isChecked())
        {
            if (event.noteOn)
            {
                const InstrumentTrack* track = selectedTrack();
                if (track != nullptr && track->instrumentType != InstrumentType::Audio)
                    audioEngine.triggerLiveMidiNoteOn(event.channel, event.pitch, event.velocity, *track);
            }
            else
            {
                audioEngine.triggerLiveMidiNoteOff(event.channel, event.pitch);
            }
        }

        if (!midiRecordingActive)
            continue;
        const int key = event.channel * 128 + event.pitch;
        if (event.noteOn)
        {
            if (static_cast<quint64>(recordedMidiNotes.size()) + midiOpenNoteCount >= midiAvailableNoteSlots)
            {
                finishMidiRecording(true);
                statusBar()->showMessage(tr("MIDI recording stopped at the pattern note limit."), 5000);
                return;
            }
            OpenMidiNote note;
            note.pitch = event.pitch;
            note.velocity = std::clamp(event.velocity, 1, 127);
            note.channel = event.channel;
            note.startBeat = currentMidiRecordingBeat();
            openMidiNotes[key].push_back(note);
            ++midiOpenNoteCount;
        }
        else
        {
            auto open = openMidiNotes.find(key);
            if (open == openMidiNotes.end() || open->isEmpty())
                continue;
            const OpenMidiNote note = open->takeLast();
            if (open->isEmpty())
                openMidiNotes.erase(open);
            --midiOpenNoteCount;
            const double endBeat = currentMidiRecordingBeat();
            recordedMidiNotes.push_back({0, note.pitch, note.velocity, note.startBeat,
                                         std::max(1.0 / 64.0, endBeat - note.startBeat), note.channel});
        }
    }
}

double MainWindow::currentMidiRecordingBeat()
{
    if (midiRecordClock.isValid())
    {
        const double elapsedSeconds = static_cast<double>(midiRecordClock.nsecsElapsed()) / 1.0e9;
        const double elapsedBeat = project.beatAtSeconds(
            project.secondsAtBeat(midiRecordStartBeat) + elapsedSeconds);
        midiLastBeat = std::max(midiLastBeat, elapsedBeat);
    }
    return midiLastBeat;
}

void MainWindow::finishMidiRecording(bool keepTake)
{
    if (!midiRecordingActive && !midiRecordingPending)
        return;

    const bool wasPending = midiRecordingPending;
    if (midiRecordingActive)
    {
        const double endBeat = currentMidiRecordingBeat();
        for (auto notes = openMidiNotes.begin(); notes != openMidiNotes.end(); ++notes)
        {
            for (const OpenMidiNote& open : notes.value())
            {
                if (static_cast<quint64>(recordedMidiNotes.size()) >= midiAvailableNoteSlots)
                    break;
                recordedMidiNotes.push_back({0, open.pitch, open.velocity, open.startBeat,
                                             std::max(1.0 / 64.0, endBeat - open.startBeat), open.channel});
            }
        }
    }

    midiRecordingActive = false;
    midiRecordingPending = false;
    if (!midiMonitorAction->isChecked() && !midiControllerListenerEnabled)
    {
        if (midiInputPollTimer != nullptr)
            midiInputPollTimer->stop();
        if (midiInput != nullptr)
            midiInput->disconnect();
        activeMidiInputPortId.clear();
        audioEngine.stopLiveMidiNotes();
    }
    else if (midiInputPollTimer != nullptr)
    {
        midiInputPollTimer->start();
    }
    openMidiNotes.clear();
    midiOpenNoteCount = 0;
    if (midiTransportActive)
    {
        audioEngine.stopPlayback();
        playAction->setChecked(false);
    }
    midiTransportActive = false;
    tempoControl->setEnabled(true);
    timeSignatureControl->setEnabled(true);
    countInControl->setEnabled(true);
    loopAction->setEnabled(true);
    metronomeAction->setEnabled(true);
    {
        const QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
    }
    refreshAudioInputs();
    refreshMidiInputs();

    if (!keepTake)
    {
        recordedMidiNotes.clear();
        recordedMidiPitchBends.clear();
        return;
    }
    if (wasPending)
    {
        statusBar()->showMessage(tr("MIDI recording stopped before the count-in finished; no notes were added."), 3500);
        recordedMidiNotes.clear();
        recordedMidiPitchBends.clear();
        return;
    }
    if (recordedMidiNotes.isEmpty() && recordedMidiPitchBends.isEmpty())
    {
        statusBar()->showMessage(tr("No MIDI notes or pitch bends were captured."), 3500);
        return;
    }

    InstrumentTrack* track = project.findTrack(midiRecordingTrackId);
    if (track == nullptr || track->instrumentType == InstrumentType::Audio)
    {
        statusBar()->showMessage(tr("The target instrument track is no longer available."), 5000);
        recordedMidiNotes.clear();
        recordedMidiPitchBends.clear();
        return;
    }
    MidiClip* clip = midiRecordingClipId == 0
        ? nullptr : project.findClip(midiRecordingTrackId, midiRecordingClipId);
    bool createdClip = false;
    if (clip == nullptr)
    {
        if (track->clips.size() >= 10000)
        {
            statusBar()->showMessage(tr("The target track has reached the 10,000 pattern limit."), 5000);
            recordedMidiNotes.clear();
            recordedMidiPitchBends.clear();
            return;
        }
        MidiClip newClip;
        newClip.id = project.nextClipId();
        newClip.name = tr("MIDI Take");
        newClip.lengthBeats = project.patternLengthBeats();
        track->clips.push_back(std::move(newClip));
        clip = &track->clips.last();
        createdClip = true;
    }

    const QVector<MidiNote> beforeNotes = clip->notes;
    const QVector<MidiPitchBend> beforePitchBends = clip->pitchBends;
    const double beforeLength = clip->lengthBeats;
    quint64 nextNoteId = project.nextNoteId();
    double maximumEndBeat = clip->lengthBeats;
    std::sort(recordedMidiNotes.begin(), recordedMidiNotes.end(), [](const MidiNote& left, const MidiNote& right)
    {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });
    for (MidiNote note : recordedMidiNotes)
    {
        if (static_cast<quint64>(clip->notes.size()) >= 100000)
            break;
        if (note.startBeat >= 16384.0)
            continue;
        note.id = nextNoteId++;
        note.startBeat = std::clamp(note.startBeat, 0.0, 16384.0 - 1.0 / 64.0);
        note.lengthBeats = std::clamp(note.lengthBeats, 1.0 / 64.0,
                                      16384.0 - note.startBeat);
        maximumEndBeat = std::max(maximumEndBeat, note.startBeat + note.lengthBeats);
        clip->notes.push_back(note);
    }
    std::stable_sort(recordedMidiPitchBends.begin(), recordedMidiPitchBends.end(),
        [](const MidiPitchBend& left, const MidiPitchBend& right)
        {
            return left.beat < right.beat;
        });
    for (MidiPitchBend bend : recordedMidiPitchBends)
    {
        if (clip->pitchBends.size() >= 100000)
            break;
        if (bend.beat >= 16384.0)
            continue;
        bend.beat = std::clamp(bend.beat, 0.0, 16384.0);
        maximumEndBeat = std::max(maximumEndBeat, bend.beat);
        clip->pitchBends.push_back(bend);
    }
    if (clip->notes.size() == beforeNotes.size() &&
        clip->pitchBends.size() == beforePitchBends.size())
    {
        if (createdClip)
            track->clips.removeLast();
        statusBar()->showMessage(tr("No MIDI notes or pitch bends fit in the target pattern."), 3500);
        recordedMidiNotes.clear();
        recordedMidiPitchBends.clear();
        return;
    }
    std::sort(clip->notes.begin(), clip->notes.end(), [](const MidiNote& left, const MidiNote& right)
    {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });
    std::stable_sort(clip->pitchBends.begin(), clip->pitchBends.end(),
        [](const MidiPitchBend& left, const MidiPitchBend& right)
        {
            return left.beat < right.beat;
        });

    const double barLength = project.quarterNotesPerBar();
    const double afterLength = std::clamp(std::ceil(maximumEndBeat / barLength) * barLength,
                                          std::max(0.25, beforeLength), 16384.0);
    clip->lengthBeats = afterLength;
    const quint64 targetClipId = clip->id;
    const quint64 targetTrackId = track->id;
    const InstrumentType targetInstrumentType = track->instrumentType;
    const QString targetClipName = clip->name;
    const QVector<MidiNote> afterNotes = clip->notes;
    const QVector<MidiPitchBend> afterPitchBends = clip->pitchBends;
    midiRecordingTrackId = 0;
    midiRecordingClipId = 0;
    selectedTrackId = targetTrackId;
    selectedClipId = targetClipId;
    if (createdClip)
        settingsDirty = true;
    refreshProjectUi();
    undoStack.push(new NoteEditCommand(this, targetTrackId, targetClipId,
                                       beforeNotes, afterNotes, beforeLength, afterLength,
                                       beforePitchBends, afterPitchBends, true,
                                       tr("Record MIDI take")));
    editorTabs->setCurrentWidget(targetInstrumentType == InstrumentType::DrumRack
                                     ? static_cast<QWidget*>(stepSequencer)
                                     : static_cast<QWidget*>(midiEditor));
    refreshDirtyState();
    statusBar()->showMessage(tr("Recorded %1 MIDI note(s) and %2 pitch bend event(s) into %3")
                                 .arg(afterNotes.size() - beforeNotes.size())
                                 .arg(afterPitchBends.size() - beforePitchBends.size())
                                 .arg(targetClipName), 4500);
    recordedMidiNotes.clear();
    recordedMidiPitchBends.clear();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!maybeSave())
    {
        event->ignore();
        return;
    }

    audioEngine.stopPlayback();
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    event->accept();
}
