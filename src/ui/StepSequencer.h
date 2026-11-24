#pragma once

#include "core/Project.h"

#include <QWidget>

class QSpinBox;
class QComboBox;
class QTableWidget;
class QLabel;
class QPushButton;

class StepSequencer final : public QWidget
{
    Q_OBJECT

public:
    explicit StepSequencer(QWidget* parent = nullptr);

    void setPattern(const QVector<MidiNote>& notes, double clipLengthBeats,
                    double quarterNotesPerBar, quint64 nextNoteId, bool available,
                    int stepsPerBar = 16,
                    const QVector<TimeSignatureChange>& meterChanges = {},
                    double clipStartBeat = 0.0);

signals:
    void notesEdited(QVector<MidiNote> before, QVector<MidiNote> after);
    void stepsPerBarChanged(int before, int after);
    void previewRequested(int midiPitch);
    void liveChainRequested();
    void liveChainStopRequested();

public:
    void setLiveChainPlaying(bool playing);

private:
    void configureDivision(int steps);
    void refreshPads();
    void toggleStep(int row, int column, bool enabled);
    void setStepVelocity(int row, int column, int velocity);
    void setStepProbability(int row, int column, int probabilityPercent);
    void setStepRatchetCount(int row, int column, int ratchetCount);
    double barStartBeat(int barIndex) const;
    double meterLengthAt(double beat) const;
    int totalBars() const;

    QSpinBox* barControl = nullptr;
    QComboBox* divisionControl = nullptr;
    QTableWidget* grid = nullptr;
    QLabel* emptyHint = nullptr;
    QPushButton* liveChainButton = nullptr;
    QVector<MidiNote> notes;
    double clipLengthBeats = 16.0;
    double quarterNotesPerBar = 4.0;
    QVector<TimeSignatureChange> localTimeSignatureChanges;
    quint64 nextNoteId = 1;
    int stepsPerBar = 16;
    bool patternAvailable = false;
};
