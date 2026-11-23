#pragma once

#include "core/Project.h"

#include <QScrollArea>

class MidiEditor final : public QScrollArea
{
    Q_OBJECT

public:
    explicit MidiEditor(QWidget* parent = nullptr);

    void setNotes(const QVector<MidiNote>& notes);
    void setPatternLengthBeats(double beats);
    void setTimeSignature(int beatsPerBar, int beatUnit);
    void setSnap(double beatStep);
    void setPixelsPerBeat(int pixels);
    void setPlayheadBeat(double beat);

signals:
    void notesEdited(QVector<MidiNote> before, QVector<MidiNote> after);
    void previewRequested(int midiPitch);

private:
    class Canvas;
    Canvas* canvas = nullptr;
};
