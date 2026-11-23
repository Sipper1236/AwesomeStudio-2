#pragma once

#include "core/Project.h"

#include <QWidget>

class QSpinBox;
class QTableWidget;
class QLabel;

class StepSequencer final : public QWidget
{
    Q_OBJECT

public:
    explicit StepSequencer(QWidget* parent = nullptr);

    void setPattern(const QVector<MidiNote>& notes, double clipLengthBeats,
                    double quarterNotesPerBar, quint64 nextNoteId, bool available);

signals:
    void notesEdited(QVector<MidiNote> before, QVector<MidiNote> after);
    void previewRequested(int midiPitch);

private:
    void refreshPads();
    void toggleStep(int row, int column, bool enabled);

    QSpinBox* barControl = nullptr;
    QTableWidget* grid = nullptr;
    QLabel* emptyHint = nullptr;
    QVector<MidiNote> notes;
    double clipLengthBeats = 16.0;
    double quarterNotesPerBar = 4.0;
    quint64 nextNoteId = 1;
    bool patternAvailable = false;
};
