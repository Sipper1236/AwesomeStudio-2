#pragma once

#include "core/Project.h"

#include <QScrollArea>

class ArrangementEditor final : public QScrollArea
{
    Q_OBJECT

public:
    explicit ArrangementEditor(QWidget* parent = nullptr);

    void setProject(const ProjectData& project);
    void setSelection(quint64 trackId, quint64 clipId);
    void setPixelsPerBeat(int pixels);
    void setPlayheadBeat(double beat);
    void setPunchRange(bool enabled, double punchInBeat, double punchOutBeat);
    void setAutomationView(bool enabled, int parameter, float minimum = 0.0f,
                           float maximum = 1.0f, float defaultValue = 0.0f,
                           const QString& pluginUri = QString(),
                           AutomationPoint::Curve defaultCurve = AutomationPoint::Curve::Linear,
                           const QString& targetLabel = QString());

signals:
    void clipSelected(quint64 trackId, quint64 clipId);
    void clipActivated(quint64 trackId, quint64 clipId);
    void clipAdded(quint64 trackId, double startBeat);
    void clipDuplicateRequested(quint64 trackId, quint64 clipId);
    void clipRemoved(quint64 trackId, quint64 clipId);
    void clipMoved(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat);
    void audioRegionTrimmed(quint64 trackId, quint64 clipId, double startBeat,
                            double lengthBeats, qint64 audioStartFrame);
    void audioFadeCurveDrawn(quint64 trackId, quint64 clipId, bool fadeIn,
                             const QVector<float>& before, const QVector<float>& after);
    void audioFadeBezierHandlesChanged(quint64 trackId, quint64 clipId, bool fadeIn,
                                       const QVector<QPointF>& before,
                                       const QVector<QPointF>& after);
    void audioRegionRelinkRequested(quint64 trackId, quint64 clipId);
    void audioTakeGroupRequested(quint64 trackId, quint64 clipId);
    void audioTakeUngroupRequested(quint64 trackId, quint64 clipId);
    void audioTakeSelected(quint64 trackId, quint64 clipId);
    void audioTakeSplitRequested(quint64 trackId, quint64 clipId, double beat);
    void trackSelected(quint64 trackId);
    void automationChanged(quint64 trackId, int parameter, const QString& pluginUri,
                           const QVector<AutomationPoint>& before,
                           const QVector<AutomationPoint>& after);
    void automationPointLimitReached();
    void markerAddRequested(double beat);
    void markerRenameRequested(double beat);
    void markerRemoveRequested(double beat);
    void markerSeekRequested(double beat);
    void tempoChangeRequested(double beat);
    void tempoRemoveRequested(double beat);
    void tempoRampToggleRequested(double beat);

private:
    class Canvas;
    Canvas* canvas = nullptr;
};
