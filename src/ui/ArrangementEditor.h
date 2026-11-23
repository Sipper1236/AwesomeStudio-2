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

signals:
    void clipSelected(quint64 trackId, quint64 clipId);
    void clipActivated(quint64 trackId, quint64 clipId);
    void clipAdded(quint64 trackId, double startBeat);
    void clipRemoved(quint64 trackId, quint64 clipId);
    void clipMoved(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat);
    void trackSelected(quint64 trackId);

private:
    class Canvas;
    Canvas* canvas = nullptr;
};
