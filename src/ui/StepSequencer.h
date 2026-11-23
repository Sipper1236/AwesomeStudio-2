#pragma once

#include "core/Project.h"

#include <QWidget>

class QSpinBox;
class QComboBox;
class QTableWidget;
class QLabel;

class StepSequencer final : public QWidget
{
    Q_OBJECT

public:
    explicit StepSequencer(QWidget* parent = nullptr);

    void setPattern(const QVector<MidiNote>& notes, double clipLengthBeats,
                    double quarterNotesPerBar, quint64 nextNoteId, bool available,
                    int stepsPerBar = 16);

signals:
    void notesEdited(QVector<MidiNote> before, QVector<MidiNote> after);
    void stepsPerBarChanged(int before, int after);
    void previewRequested(int midiPitch);

private:
    void configureDivision(int steps);
    void refreshPads();
    void toggleStep(int row, int column, bool enabled);

    QSpinBox* barControl = nullptr;
    QComboBox* divisionControl = nullptr;
    QTableWidget* grid = nullptr;
    QLabel* emptyHint = nullptr;
    QVector<MidiNote> notes;
    double clipLengthBeats = 16.0;
    double quarterNotesPerBar = 4.0;
    quint64 nextNoteId = 1;
    int stepsPerBar = 16;
    bool patternAvailable = false;
};
