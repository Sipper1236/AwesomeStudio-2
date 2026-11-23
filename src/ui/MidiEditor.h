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
    void setScale(int rootPitchClass, const QVector<int>& intervals, bool snapNewNotes);
    void setGhostNotes(const QVector<MidiNote>& notes);
    void setGhostNotesEnabled(bool enabled);
    void quantizeNotesToGrid();
    bool addChordToSelection(const QVector<int>& semitoneIntervals);
    bool arpeggiateSelectedChord();
    void editSelectedNoteProperties();
    void setPixelsPerBeat(int pixels);
    void setPlayheadBeat(double beat);

signals:
    void notesEdited(QVector<MidiNote> before, QVector<MidiNote> after);
    void previewRequested(int midiPitch);

private:
    class Canvas;
    Canvas* canvas = nullptr;
};
