#include "StepSequencer.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr int stepCount = 16;
constexpr std::array<int, 4> drumPitches = {36, 38, 42, 46};
constexpr std::array<const char*, 4> drumNames = {"Kick", "Snare", "Closed Hat", "Open Hat"};
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
    auto* hint = new QLabel(tr("16 steps per bar · changes are written to the selected drum clip"), this);
    hint->setStyleSheet(QStringLiteral("color: #a8b5c1;"));
    controls->addWidget(hint, 1);
    root->addLayout(controls);

    emptyHint = new QLabel(tr("Add a pattern to this drum track in the Playlist to start programming beats."), this);
    emptyHint->setWordWrap(true);
    emptyHint->setStyleSheet(QStringLiteral("color: #a8b5c1; background: #202731; border: 1px solid #343d48; padding: 12px;"));
    emptyHint->hide();
    root->addWidget(emptyHint);

    grid = new QTableWidget(4, stepCount, this);
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
    for (int step = 0; step < stepCount; ++step)
        grid->setHorizontalHeaderItem(step, new QTableWidgetItem(QStringLiteral("%1").arg(step + 1, 2, 10, QLatin1Char('0'))));
    for (int row = 0; row < static_cast<int>(drumNames.size()); ++row)
    {
        grid->setVerticalHeaderItem(row, new QTableWidgetItem(tr(drumNames[row])));
        for (int column = 0; column < stepCount; ++column)
        {
            auto* pad = new QPushButton(grid);
            pad->setCheckable(true);
            pad->setAccessibleName(tr("%1, step %2").arg(tr(drumNames[row])).arg(column + 1));
            pad->setToolTip(tr("%1 · step %2").arg(tr(drumNames[row])).arg(column + 1));
            grid->setCellWidget(row, column, pad);
            connect(pad, &QPushButton::toggled, this,
                    [this, row, column](bool enabled) { toggleStep(row, column, enabled); });
        }
    }
    root->addWidget(grid);
    root->addStretch(1);
    connect(barControl, qOverload<int>(&QSpinBox::valueChanged), this, [this]
    {
        refreshPads();
    });
}

void StepSequencer::setPattern(const QVector<MidiNote>& newNotes, double length,
                               double barLength, quint64 noteId, bool available)
{
    notes = newNotes;
    clipLengthBeats = std::max(0.25, length);
    quarterNotesPerBar = std::max(0.25, barLength);
    nextNoteId = std::max<quint64>(1, noteId);
    patternAvailable = available;
    emptyHint->setVisible(!patternAvailable);
    barControl->setEnabled(patternAvailable);
    grid->setEnabled(patternAvailable);
    const int bars = std::clamp(static_cast<int>(std::ceil(clipLengthBeats / quarterNotesPerBar)), 1, 4096);
    const QSignalBlocker blocker(barControl);
    const int currentBar = std::clamp(barControl->value(), 1, bars);
    barControl->setRange(1, bars);
    barControl->setValue(currentBar);
    refreshPads();
}

void StepSequencer::refreshPads()
{
    const double barStart = (barControl->value() - 1) * quarterNotesPerBar;
    const double stepLength = quarterNotesPerBar / stepCount;
    for (int row = 0; row < static_cast<int>(drumPitches.size()); ++row)
    {
        for (int column = 0; column < stepCount; ++column)
        {
            const double stepStart = barStart + column * stepLength;
            bool active = false;
            for (const MidiNote& note : notes)
            {
                if (note.pitch == drumPitches[row] &&
                    std::abs(note.startBeat - stepStart) <= stepLength * 0.45)
                {
                    active = true;
                    break;
                }
            }
            auto* pad = qobject_cast<QPushButton*>(grid->cellWidget(row, column));
            if (pad == nullptr)
                continue;
            const QSignalBlocker blocker(pad);
            pad->setEnabled(patternAvailable && stepStart < clipLengthBeats);
            pad->setChecked(active);
        }
    }
}

void StepSequencer::toggleStep(int row, int column, bool enabled)
{
    if (row < 0 || row >= static_cast<int>(drumPitches.size()) || column < 0 || column >= stepCount)
        return;
    const double stepLength = quarterNotesPerBar / stepCount;
    const double stepStart = (barControl->value() - 1) * quarterNotesPerBar + column * stepLength;
    if (stepStart >= clipLengthBeats)
        return;

    const QVector<MidiNote> before = notes;
    const int pitch = drumPitches[row];
    const auto matchesStep = [pitch, stepStart, stepLength](const MidiNote& note)
    {
        return note.pitch == pitch && std::abs(note.startBeat - stepStart) <= stepLength * 0.45;
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
