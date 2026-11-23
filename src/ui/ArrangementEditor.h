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
    void setAutomationView(bool enabled, int parameter);

signals:
    void clipSelected(quint64 trackId, quint64 clipId);
    void clipActivated(quint64 trackId, quint64 clipId);
    void clipAdded(quint64 trackId, double startBeat);
    void clipRemoved(quint64 trackId, quint64 clipId);
    void clipMoved(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat);
    void audioRegionTrimmed(quint64 trackId, quint64 clipId, double startBeat,
                            double lengthBeats, qint64 audioStartFrame);
    void audioRegionRelinkRequested(quint64 trackId, quint64 clipId);
    void trackSelected(quint64 trackId);
    void automationChanged(quint64 trackId, int parameter,
                           const QVector<AutomationPoint>& before,
                           const QVector<AutomationPoint>& after);
    void automationPointLimitReached();
    void markerAddRequested(double beat);
    void markerRenameRequested(double beat);
    void markerRemoveRequested(double beat);
    void markerSeekRequested(double beat);

private:
    class Canvas;
    Canvas* canvas = nullptr;
};
