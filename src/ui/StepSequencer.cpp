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
    auto* hint = new QLabel(tr("Right-click a step to set probability or ratchets · scroll for finer divisions"), this);
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
                const double stepLength = quarterNotesPerBar / stepsPerBar;
                const double barStart = (barControl->value() - 1) * quarterNotesPerBar;
                const double barEnd = barStart + quarterNotesPerBar;
                const int pitch = drumPitches[static_cast<size_t>(row)];
                int currentProbability = 100;
                int currentRatchets = 1;
                for (const MidiNote& note : notes)
                {
                    if (matchesGridStep(note, pitch, barStart, barEnd, stepLength,
                                        stepsPerBar, column))
                    {
                        currentProbability = note.probabilityPercent;
                        currentRatchets = note.ratchetCount;
                        break;
                    }
                }
                QMenu menu(this);
                QAction* probabilityAction = menu.addAction(tr("Set probability…"));
                QAction* ratchetAction = menu.addAction(tr("Set ratchets…"));
                QAction* selectedAction = menu.exec(pad->mapToGlobal(localPosition));
                if (selectedAction == probabilityAction)
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
                               int requestedStepsPerBar)
{
    notes = newNotes;
    clipLengthBeats = std::max(0.25, length);
    quarterNotesPerBar = std::max(0.25, barLength);
    nextNoteId = std::max<quint64>(1, noteId);
    patternAvailable = available;
    configureDivision(requestedStepsPerBar);
    emptyHint->setVisible(!patternAvailable);
    barControl->setEnabled(patternAvailable);
    divisionControl->setEnabled(patternAvailable);
    grid->setEnabled(patternAvailable);
    const int bars = std::clamp(static_cast<int>(std::ceil(clipLengthBeats / quarterNotesPerBar)), 1, 4096);
    const QSignalBlocker blocker(barControl);
    const int currentBar = std::clamp(barControl->value(), 1, bars);
    barControl->setRange(1, bars);
    barControl->setValue(currentBar);
    refreshPads();
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
    const double barStart = (barControl->value() - 1) * quarterNotesPerBar;
    const double barEnd = barStart + quarterNotesPerBar;
    const double stepLength = quarterNotesPerBar / stepsPerBar;
    for (int row = 0; row < static_cast<int>(drumPitches.size()); ++row)
    {
        for (int column = 0; column < stepsPerBar; ++column)
        {
            const double stepStart = barStart + column * stepLength;
            bool active = false;
            int probability = 100;
            int ratchets = 1;
            for (const MidiNote& note : notes)
            {
                if (matchesGridStep(note, drumPitches[row], barStart, barEnd,
                                    stepLength, stepsPerBar, column))
                {
                    active = true;
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
                                                           : QString());
            QStringList details;
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
    const double stepLength = quarterNotesPerBar / stepsPerBar;
    const double barStart = (barControl->value() - 1) * quarterNotesPerBar;
    const double barEnd = barStart + quarterNotesPerBar;
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
    const double stepLength = quarterNotesPerBar / stepsPerBar;
    const double barStart = (barControl->value() - 1) * quarterNotesPerBar;
    const double barEnd = barStart + quarterNotesPerBar;
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

void StepSequencer::setStepRatchetCount(int row, int column, int ratchetCount)
{
    if (!patternAvailable || row < 0 || row >= static_cast<int>(drumPitches.size()) ||
        column < 0 || column >= stepsPerBar)
        return;
    const double stepLength = quarterNotesPerBar / stepsPerBar;
    const double barStart = (barControl->value() - 1) * quarterNotesPerBar;
    const double barEnd = barStart + quarterNotesPerBar;
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
