#include "StepSequencer.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QInputDialog>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr int maximumStepCount = 64;
constexpr std::array<int, 7> supportedStepCounts = {8, 12, 16, 24, 32, 48, 64};
constexpr std::array<int, 4> drumPitches = {36, 38, 42, 46};
constexpr std::array<const char*, 4> drumNames = {"Kick", "Snare", "Closed Hat", "Open Hat"};

int supportedStepCount(int requested)
{
    return *std::min_element(supportedStepCounts.cbegin(), supportedStepCounts.cend(),
                             [requested](int left, int right)
    {
        return std::abs(left - requested) < std::abs(right - requested);
    });
}

bool matchesGridStep(const MidiNote& note, int pitch, double barStart, double barEnd,
                    double stepLength, int stepsPerBar, int column)
{
    const double relativeBeat = note.startBeat - barStart;
    return note.pitch == pitch && note.startBeat >= barStart && note.startBeat < barEnd &&
           std::clamp(static_cast<int>(std::floor(relativeBeat / stepLength + 0.5)),
                      0, stepsPerBar - 1) == column;
}
}

StepSequencer::StepSequencer(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("stepSequencer"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 16);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("Step sequencer"), this);
    heading->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 650; color: #e8edf2;"));
    root->addWidget(heading);

    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    controls->addWidget(new QLabel(tr("BAR"), this));
    barControl = new QSpinBox(this);
    barControl->setRange(1, 1);
    barControl->setFixedWidth(72);
    barControl->setAccessibleName(tr("Sequencer bar"));
    controls->addWidget(barControl);
    controls->addWidget(new QLabel(tr("STEPS / BAR"), this));
    divisionControl = new QComboBox(this);
    divisionControl->setObjectName(QStringLiteral("stepDivisionControl"));
    divisionControl->setAccessibleName(tr("Steps per bar"));
    divisionControl->setFixedWidth(76);
    for (const int division : supportedStepCounts)
        divisionControl->addItem(QString::number(division), division);
    controls->addWidget(divisionControl);
    auto* hint = new QLabel(tr("Right-click a step to set velocity, probability, or ratchets · scroll for finer divisions"), this);
    hint->setStyleSheet(QStringLiteral("color: #a8b5c1;"));
    controls->addWidget(hint, 1);
    root->addLayout(controls);

    emptyHint = new QLabel(tr("Add a pattern to this drum track in the Playlist to start programming beats."), this);
    emptyHint->setWordWrap(true);
    emptyHint->setStyleSheet(QStringLiteral("color: #a8b5c1; background: #202731; border: 1px solid #343d48; padding: 12px;"));
    emptyHint->hide();
    root->addWidget(emptyHint);

    grid = new QTableWidget(4, maximumStepCount, this);
    grid->setObjectName(QStringLiteral("drumStepGrid"));
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->setSelectionMode(QAbstractItemView::NoSelection);
    grid->setFocusPolicy(Qt::NoFocus);
    grid->verticalHeader()->setVisible(true);
    grid->verticalHeader()->setFixedWidth(106);
    grid->verticalHeader()->setDefaultSectionSize(52);
    grid->horizontalHeader()->setVisible(true);
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    grid->horizontalHeader()->setDefaultSectionSize(38);
    grid->horizontalHeader()->setMinimumSectionSize(32);
    grid->horizontalHeader()->setFixedHeight(32);
    grid->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid->setShowGrid(false);
    grid->setMinimumHeight(250);
    grid->setMaximumHeight(278);
    grid->setStyleSheet(QStringLiteral(
        "QTableWidget { background: #151a21; border: 1px solid #343d48; color: #dce4eb; }"
        "QHeaderView::section { background: #202731; color: #aeb9c4; border: 0; padding: 5px; font-weight: 600; }"
        "QPushButton { background: #242c35; color: #c9d2dc; border: 1px solid #3a4653; border-radius: 4px; min-height: 34px; }"
        "QPushButton:hover { background: #303b47; border-color: #71808d; }"
        "QPushButton:focus { border: 2px solid #b3fff3; }"
        "QPushButton:checked { background: #56d3c0; color: #102626; border-color: #8be9dc; }"
        "QPushButton:disabled { background: #1d232a; color: #68737e; border-color: #303741; }"));
    for (int step = 0; step < maximumStepCount; ++step)
        grid->setHorizontalHeaderItem(step, new QTableWidgetItem(QStringLiteral("%1").arg(step + 1, 2, 10, QLatin1Char('0'))));
    for (int row = 0; row < static_cast<int>(drumNames.size()); ++row)
    {
        grid->setVerticalHeaderItem(row, new QTableWidgetItem(tr(drumNames[row])));
        for (int column = 0; column < maximumStepCount; ++column)
        {
            auto* pad = new QPushButton(grid);
            pad->setCheckable(true);
            pad->setAccessibleName(tr("%1, step %2").arg(tr(drumNames[row])).arg(column + 1));
            pad->setToolTip(tr("%1 · step %2").arg(tr(drumNames[row])).arg(column + 1));
            pad->setContextMenuPolicy(Qt::CustomContextMenu);
            grid->setCellWidget(row, column, pad);
            connect(pad, &QPushButton::toggled, this,
                    [this, row, column](bool enabled) { toggleStep(row, column, enabled); });
            connect(pad, &QWidget::customContextMenuRequested, this,
                    [this, row, column, pad](const QPoint& localPosition)
            {
                if (!patternAvailable)
                    return;
                const double barStart = barStartBeat(barControl->value() - 1);
                const double barLength = meterLengthAt(barStart);
                const double stepLength = barLength / stepsPerBar;
                const double barEnd = barStart + barLength;
                const int pitch = drumPitches[static_cast<size_t>(row)];
                int currentVelocity = 100;
                int currentProbability = 100;
                int currentRatchets = 1;
                for (const MidiNote& note : notes)
                {
                    if (matchesGridStep(note, pitch, barStart, barEnd, stepLength,
                                        stepsPerBar, column))
                    {
                        currentVelocity = note.velocity;
                        currentProbability = note.probabilityPercent;
                        currentRatchets = note.ratchetCount;
                        break;
                    }
                }
                QMenu menu(this);
                QAction* velocityAction = menu.addAction(tr("Set velocity…"));
                QAction* probabilityAction = menu.addAction(tr("Set probability…"));
                QAction* ratchetAction = menu.addAction(tr("Set ratchets…"));
                QAction* selectedAction = menu.exec(pad->mapToGlobal(localPosition));
                if (selectedAction == velocityAction)
                {
                    bool accepted = false;
                    const int velocity = QInputDialog::getInt(this, tr("Step velocity"),
                        tr("Velocity for %1, step %2 (1–127)")
                            .arg(tr(drumNames[row])).arg(column + 1),
                        currentVelocity, 1, 127, 1, &accepted);
                    if (accepted)
                        setStepVelocity(row, column, velocity);
                }
                else if (selectedAction == probabilityAction)
                {
                    bool accepted = false;
                    const int probability = QInputDialog::getInt(this, tr("Step probability"),
                        tr("Trigger chance for %1, step %2 (%)")
                            .arg(tr(drumNames[row])).arg(column + 1),
                        currentProbability, 0, 100, 5, &accepted);
                    if (accepted)
                        setStepProbability(row, column, probability);
                }
                else if (selectedAction == ratchetAction)
                {
                    bool accepted = false;
                    const int ratchets = QInputDialog::getInt(this, tr("Step ratchets"),
                        tr("Hits for %1, step %2")
                            .arg(tr(drumNames[row])).arg(column + 1),
                        currentRatchets, 1, 8, 1, &accepted);
                    if (accepted)
                        setStepRatchetCount(row, column, ratchets);
                }
            });
        }
    }
    root->addWidget(grid);
    root->addStretch(1);
    connect(barControl, qOverload<int>(&QSpinBox::valueChanged), this, [this]
    {
        refreshPads();
    });
    connect(divisionControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        if (index < 0)
            return;
        const int requested = divisionControl->itemData(index).toInt();
        if (requested == stepsPerBar)
            return;
        const int before = stepsPerBar;
        configureDivision(requested);
        refreshPads();
        emit stepsPerBarChanged(before, stepsPerBar);
    });
}

void StepSequencer::setPattern(const QVector<MidiNote>& newNotes, double length,
                               double barLength, quint64 noteId, bool available,
                               int requestedStepsPerBar,
                               const QVector<TimeSignatureChange>& meterChanges,
                               double clipStartBeat)
{
    notes = newNotes;
    clipLengthBeats = std::max(0.25, length);
    quarterNotesPerBar = std::max(0.25, barLength);
    localTimeSignatureChanges.clear();
    TimeSignatureChange active{0.0, 4, 4};
    constexpr int denominators[] = {1, 2, 4, 8, 16};
    for (int denominator : denominators)
        for (int numerator = 1; numerator <= 12; ++numerator)
            if (std::abs(static_cast<double>(numerator) * 4.0 / denominator - quarterNotesPerBar) < 1.0e-8)
            {
                active.beatsPerBar = numerator;
                active.beatUnit = denominator;
            }
    auto nextMeter = std::upper_bound(meterChanges.cbegin(), meterChanges.cend(), clipStartBeat,
        [](double beat, const TimeSignatureChange& change) { return beat < change.beat; });
    if (nextMeter != meterChanges.cbegin())
        active = *(nextMeter - 1);
    active.beat = 0.0;
    localTimeSignatureChanges.push_back(active);
    for (; nextMeter != meterChanges.cend(); ++nextMeter)
        localTimeSignatureChanges.push_back({nextMeter->beat - clipStartBeat,
            nextMeter->beatsPerBar, nextMeter->beatUnit});
    nextNoteId = std::max<quint64>(1, noteId);
    patternAvailable = available;
    configureDivision(requestedStepsPerBar);
    emptyHint->setVisible(!patternAvailable);
    barControl->setEnabled(patternAvailable);
    divisionControl->setEnabled(patternAvailable);
    grid->setEnabled(patternAvailable);
    const int bars = totalBars();
    const QSignalBlocker blocker(barControl);
    const int currentBar = std::clamp(barControl->value(), 1, bars);
    barControl->setRange(1, bars);
    barControl->setValue(currentBar);
    refreshPads();
}

double StepSequencer::meterLengthAt(double beat) const
{
    const auto next = std::upper_bound(localTimeSignatureChanges.cbegin(),
        localTimeSignatureChanges.cend(), beat,
        [](double value, const TimeSignatureChange& change) { return value < change.beat; });
    const TimeSignatureChange& meter = next == localTimeSignatureChanges.cbegin()
        ? localTimeSignatureChanges.first() : *(next - 1);
    return static_cast<double>(meter.beatsPerBar) * 4.0 / meter.beatUnit;
}

double StepSequencer::barStartBeat(int barIndex) const
{
    double beat = 0.0;
    for (int bar = 0; bar < barIndex; ++bar)
    {
        double next = beat + meterLengthAt(beat);
        for (const TimeSignatureChange& change : localTimeSignatureChanges)
            if (change.beat > beat + 1.0e-9 && change.beat < next)
            {
                next = change.beat;
                break;
            }
        beat = next;
    }
    return beat;
}

int StepSequencer::totalBars() const
{
    int bars = 0;
    double beat = 0.0;
    while (beat < clipLengthBeats && bars < 4096)
    {
        double next = beat + meterLengthAt(beat);
        for (const TimeSignatureChange& change : localTimeSignatureChanges)
            if (change.beat > beat + 1.0e-9 && change.beat < next)
            {
                next = change.beat;
                break;
            }
        if (next <= beat + 1.0e-9)
            break;
        beat = next;
        ++bars;
    }
    return std::max(1, bars);
}

void StepSequencer::configureDivision(int requestedSteps)
{
    stepsPerBar = supportedStepCount(requestedSteps);
    const QSignalBlocker blocker(divisionControl);
    divisionControl->setCurrentIndex(divisionControl->findData(stepsPerBar));
    for (int column = 0; column < maximumStepCount; ++column)
        grid->setColumnHidden(column, column >= stepsPerBar);
}

void StepSequencer::refreshPads()
{
    const double barStart = barStartBeat(barControl->value() - 1);
    const double barLength = meterLengthAt(barStart);
    const double barEnd = barStart + barLength;
    const double stepLength = barLength / stepsPerBar;
    for (int row = 0; row < static_cast<int>(drumPitches.size()); ++row)
    {
        for (int column = 0; column < stepsPerBar; ++column)
        {
            const double stepStart = barStart + column * stepLength;
            bool active = false;
            int velocity = 100;
            int probability = 100;
            int ratchets = 1;
            for (const MidiNote& note : notes)
            {
                if (matchesGridStep(note, drumPitches[row], barStart, barEnd,
                                    stepLength, stepsPerBar, column))
                {
                    active = true;
                    velocity = note.velocity;
                    probability = note.probabilityPercent;
                    ratchets = note.ratchetCount;
                    break;
                }
            }
            auto* pad = qobject_cast<QPushButton*>(grid->cellWidget(row, column));
            if (pad == nullptr)
                continue;
            const QSignalBlocker blocker(pad);
            pad->setEnabled(patternAvailable && stepStart < clipLengthBeats);
            pad->setChecked(active);
            pad->setText(active && ratchets > 1
                             ? QStringLiteral("%1x").arg(ratchets)
                             : active && probability < 100 ? QStringLiteral("%1%").arg(probability)
                             : active && velocity != 100 ? QStringLiteral("v%1").arg(velocity)
                                                         : QString());
            QStringList details;
            if (active)
                details.push_back(tr("Velocity %1 / 127").arg(velocity));
            if (active && probability < 100)
                details.push_back(tr("%1% trigger chance").arg(probability));
            if (active && ratchets > 1)
                details.push_back(tr("%1 hits").arg(ratchets));
            pad->setToolTip(details.isEmpty()
                ? tr("%1 · step %2").arg(tr(drumNames[row])).arg(column + 1)
                : tr("%1 · step %2 · %3").arg(tr(drumNames[row])).arg(column + 1)
                    .arg(details.join(QStringLiteral(" · "))));
        }
    }
}

void StepSequencer::toggleStep(int row, int column, bool enabled)
{
    if (row < 0 || row >= static_cast<int>(drumPitches.size()) || column < 0 || column >= stepsPerBar)
        return;
    const double barStart = barStartBeat(barControl->value() - 1);
    const double barLength = meterLengthAt(barStart);
    const double barEnd = barStart + barLength;
    const double stepLength = barLength / stepsPerBar;
    const double stepStart = barStart + column * stepLength;
    if (stepStart >= clipLengthBeats)
        return;

    const QVector<MidiNote> before = notes;
    const int pitch = drumPitches[row];
    const auto matchesStep = [pitch, barStart, barEnd, stepLength, column, this](const MidiNote& note)
    {
        return matchesGridStep(note, pitch, barStart, barEnd, stepLength, stepsPerBar, column);
    };
    if (enabled)
    {
        const bool exists = std::any_of(notes.cbegin(), notes.cend(), matchesStep);
        if (!exists)
        {
            MidiNote note;
            note.id = nextNoteId++;
            note.pitch = pitch;
            note.velocity = 100;
            note.startBeat = stepStart;
            note.lengthBeats = std::max(1.0 / 64.0, stepLength);
            notes.push_back(note);
        }
        emit previewRequested(pitch);
    }
    else
    {
        notes.erase(std::remove_if(notes.begin(), notes.end(), matchesStep), notes.end());
    }
    refreshPads();
    if (notes != before)
        emit notesEdited(before, notes);
}

void StepSequencer::setStepProbability(int row, int column, int probabilityPercent)
{
    if (!patternAvailable || row < 0 || row >= static_cast<int>(drumPitches.size()) ||
        column < 0 || column >= stepsPerBar)
        return;
    const double barStart = barStartBeat(barControl->value() - 1);
    const double barLength = meterLengthAt(barStart);
    const double barEnd = barStart + barLength;
    const double stepLength = barLength / stepsPerBar;
    const double stepStart = barStart + column * stepLength;
    if (stepStart >= clipLengthBeats)
        return;

    const QVector<MidiNote> before = notes;
    const int pitch = drumPitches[static_cast<size_t>(row)];
    bool found = false;
    for (MidiNote& note : notes)
    {
        if (!matchesGridStep(note, pitch, barStart, barEnd, stepLength, stepsPerBar, column))
            continue;
        note.probabilityPercent = std::clamp(probabilityPercent, 0, 100);
        found = true;
    }
    if (!found)
    {
        MidiNote note;
        note.id = nextNoteId++;
        note.pitch = pitch;
        note.velocity = 100;
        note.probabilityPercent = std::clamp(probabilityPercent, 0, 100);
        note.startBeat = stepStart;
        note.lengthBeats = std::max(1.0 / 64.0, stepLength);
        notes.push_back(note);
        emit previewRequested(pitch);
    }
    refreshPads();
    if (notes != before)
        emit notesEdited(before, notes);
}

void StepSequencer::setStepVelocity(int row, int column, int velocity)
{
    if (!patternAvailable || row < 0 || row >= static_cast<int>(drumPitches.size()) ||
        column < 0 || column >= stepsPerBar)
        return;
    const double barStart = barStartBeat(barControl->value() - 1);
    const double barLength = meterLengthAt(barStart);
    const double barEnd = barStart + barLength;
    const double stepLength = barLength / stepsPerBar;
    const double stepStart = barStart + column * stepLength;
    if (stepStart >= clipLengthBeats)
        return;

    const QVector<MidiNote> before = notes;
    const int pitch = drumPitches[static_cast<size_t>(row)];
    bool found = false;
    for (MidiNote& note : notes)
    {
        if (!matchesGridStep(note, pitch, barStart, barEnd, stepLength, stepsPerBar, column))
            continue;
        note.velocity = std::clamp(velocity, 1, 127);
        found = true;
    }
    if (!found)
    {
        MidiNote note;
        note.id = nextNoteId++;
        note.pitch = pitch;
        note.velocity = std::clamp(velocity, 1, 127);
        note.startBeat = stepStart;
        note.lengthBeats = std::max(1.0 / 64.0, stepLength);
        notes.push_back(note);
        emit previewRequested(pitch);
    }
    refreshPads();
    if (notes != before)
        emit notesEdited(before, notes);
}

void StepSequencer::setStepRatchetCount(int row, int column, int ratchetCount)
{
    if (!patternAvailable || row < 0 || row >= static_cast<int>(drumPitches.size()) ||
        column < 0 || column >= stepsPerBar)
        return;
    const double barStart = barStartBeat(barControl->value() - 1);
    const double barLength = meterLengthAt(barStart);
    const double barEnd = barStart + barLength;
    const double stepLength = barLength / stepsPerBar;
    const double stepStart = barStart + column * stepLength;
    if (stepStart >= clipLengthBeats)
        return;

    const QVector<MidiNote> before = notes;
    const int pitch = drumPitches[static_cast<size_t>(row)];
    bool found = false;
    for (MidiNote& note : notes)
    {
        if (!matchesGridStep(note, pitch, barStart, barEnd, stepLength, stepsPerBar, column))
            continue;
        note.ratchetCount = std::clamp(ratchetCount, 1, 8);
        found = true;
    }
    if (!found)
    {
        MidiNote note;
        note.id = nextNoteId++;
        note.pitch = pitch;
        note.velocity = 100;
        note.ratchetCount = std::clamp(ratchetCount, 1, 8);
        note.startBeat = stepStart;
        note.lengthBeats = std::max(1.0 / 64.0, stepLength);
        notes.push_back(note);
        emit previewRequested(pitch);
    }
    refreshPads();
    if (notes != before)
        emit notesEdited(before, notes);
}
