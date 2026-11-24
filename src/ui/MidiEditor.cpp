#include "MidiEditor.h"

#include <QKeyEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int sidebarWidth = 58;
constexpr int rulerHeight = 30;
constexpr int rowHeight = 18;
constexpr int velocityHeight = 100;
constexpr int pitchBendHeight = 78;
constexpr int lowestPitch = 0;
constexpr int highestPitch = 127;
constexpr int pitchCount = highestPitch - lowestPitch + 1;

bool isBlackKey(int pitchClass)
{
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 ||
           pitchClass == 8 || pitchClass == 10;
}

QString noteLabel(int pitch)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return QStringLiteral("%1%2").arg(QString::fromLatin1(names[pitch % 12])).arg(pitch / 12 - 1);
}

QString pitchBendRangeLabel(double semitones)
{
    QString label = QString::number(semitones, 'f', 2);
    while (label.contains('.') && label.endsWith('0'))
        label.chop(1);
    if (label.endsWith('.'))
        label.chop(1);
    return label;
}
}

class MidiEditor::Canvas final : public QWidget
{
public:
    explicit Canvas(MidiEditor* owner) : QWidget(owner), owner(owner)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        updatePitchBendToolTip();
        setLengthBeats(16.0);
    }

    void setNotes(const QVector<MidiNote>& newNotes)
    {
        notes = newNotes;
        QSet<quint64> existingIds;
        for (const MidiNote& note : notes)
            existingIds.insert(note.id);
        for (auto it = selected.cbegin(); it != selected.cend();)
        {
            if (!existingIds.contains(*it))
                it = selected.erase(it);
            else
                ++it;
        }
        update();

        int topPitch = 84;
        if (!notes.isEmpty())
        {
            int highestNote = lowestPitch;
            for (const MidiNote& note : notes)
                highestNote = std::max(highestNote, note.pitch);
            topPitch = std::min(highestPitch, highestNote + 2);
        }
        const int scrollTo = (highestPitch - topPitch) * rowHeight;
        QTimer::singleShot(0, owner, [this, scrollTo]
        {
            owner->verticalScrollBar()->setValue(scrollTo);
        });
    }

    void setPitchBends(const QVector<MidiPitchBend>& newPitchBends)
    {
        pitchBends = newPitchBends;
        std::stable_sort(pitchBends.begin(), pitchBends.end(),
            [](const MidiPitchBend& left, const MidiPitchBend& right)
            {
                return left.beat < right.beat;
            });
        selectedPitchBendIndex = -1;
        update();
    }

    void setDefaultPitchBendChannel(int channel)
    {
        defaultPitchBendChannel = std::clamp(channel, 0, 15);
    }

    void setPitchBendLaneEnabled(bool enabled)
    {
        pitchBendLaneEnabled = enabled;
        update();
    }

    void setPitchBendRange(double semitones)
    {
        pitchBendRangeSemitones = std::clamp(semitones, 0.01, 48.0);
        updatePitchBendToolTip();
        update();
    }

    void setLengthBeats(double beats)
    {
        patternLength = std::max(0.25, beats);
        updateCanvasSize();
    }

    void setTimeSignature(int numerator, int denominator)
    {
        denominator = denominator == 1 || denominator == 2 || denominator == 4 ||
                      denominator == 8 || denominator == 16 ? denominator : 4;
        quarterNotesPerBar = std::clamp(numerator, 1, 12) * 4.0 / denominator;
        update();
    }

    void setTimeSignatureMap(const QVector<TimeSignatureChange>& changes, double clipStartBeat)
    {
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
        auto next = std::upper_bound(changes.cbegin(), changes.cend(), clipStartBeat,
            [](double beat, const TimeSignatureChange& change) { return beat < change.beat; });
        if (next != changes.cbegin())
            active = *(next - 1);
        active.beat = 0.0;
        localTimeSignatureChanges.push_back(active);
        for (; next != changes.cend(); ++next)
            localTimeSignatureChanges.push_back({next->beat - clipStartBeat,
                                                 next->beatsPerBar, next->beatUnit});
        quarterNotesPerBar = static_cast<double>(active.beatsPerBar) * 4.0 / active.beatUnit;
        update();
    }

    void setSnap(double beatStep)
    {
        snapToBar = beatStep < 0.0;
        if (snapToBar)
            barSnapDivisions = std::clamp(static_cast<int>(std::lround(-beatStep)), 1, 16);
        else
            snap = std::clamp(beatStep, 1.0 / 64.0, 4.0);
    }

    void setScale(int root, const QVector<int>& intervals, bool snapNotes)
    {
        rootPitchClass = std::clamp(root, 0, 11);
        scaleIntervals.clear();
        for (const int interval : intervals)
        {
            if (interval >= 0 && interval < 12 && !scaleIntervals.contains(interval))
                scaleIntervals.push_back(interval);
        }
        std::sort(scaleIntervals.begin(), scaleIntervals.end());
        if (scaleIntervals.isEmpty())
        {
            for (int interval = 0; interval < 12; ++interval)
                scaleIntervals.push_back(interval);
        }
        snapNewNotesToScale = snapNotes;
        update();
    }

    void setGhostNotes(const QVector<MidiNote>& newGhostNotes)
    {
        ghostNotes = newGhostNotes;
        update();
    }

    void setGhostNotesEnabled(bool enabled)
    {
        ghostNotesEnabled = enabled;
        update();
    }

    void quantizeNotesToGrid()
    {
        if (notes.isEmpty())
            return;

        const QVector<MidiNote> before = notes;
        const bool quantizeAll = selected.isEmpty();
        for (MidiNote& note : notes)
        {
            if (!quantizeAll && !selected.contains(note.id))
                continue;
            note.startBeat = std::clamp(snapNearest(note.startBeat), 0.0,
                                        std::max(0.0, patternLength - note.lengthBeats));
        }
        std::sort(notes.begin(), notes.end(), [](const MidiNote& left, const MidiNote& right)
        {
            if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
            return left.pitch < right.pitch;
        });
        commitIfChanged(before);
        update();
    }

    bool addChordToSelection(const QVector<int>& semitoneIntervals)
    {
        if (selected.isEmpty() || semitoneIntervals.size() < 2)
            return false;

        const QVector<MidiNote> before = notes;
        QVector<MidiNote> roots;
        for (const MidiNote& note : notes)
        {
            if (selected.contains(note.id))
                roots.push_back(note);
        }
        if (roots.isEmpty())
            return false;

        QSet<quint64> chordSelection;
        for (const MidiNote& root : roots)
        {
            chordSelection.insert(root.id);
            for (const int interval : semitoneIntervals)
            {
                if (interval <= 0 || root.pitch + interval > 127)
                    continue;

                MidiNote chordNote = root;
                chordNote.pitch += interval;
                const auto existing = std::find_if(notes.cbegin(), notes.cend(),
                    [&chordNote](const MidiNote& note)
                    {
                        return note.pitch == chordNote.pitch &&
                               note.startBeat == chordNote.startBeat &&
                               note.lengthBeats == chordNote.lengthBeats &&
                               note.channel == chordNote.channel;
                    });
                if (existing != notes.cend())
                {
                    chordSelection.insert(existing->id);
                    continue;
                }

                chordNote.id = nextId();
                notes.push_back(chordNote);
                chordSelection.insert(chordNote.id);
            }
        }

        std::sort(notes.begin(), notes.end(), [](const MidiNote& left, const MidiNote& right)
        {
            if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
            return left.pitch < right.pitch;
        });
        selected = chordSelection;
        commitIfChanged(before);
        update();
        return true;
    }

    bool arpeggiateSelectedChord(int direction, int octaveRange, int gatePercent,
                                 int velocityPattern)
    {
        QVector<MidiNote> chord;
        for (const MidiNote& note : notes)
        {
            if (selected.contains(note.id))
                chord.push_back(note);
        }
        if (chord.size() < 2)
            return false;

        const double startBeat = chord.first().startBeat;
        const double noteLength = chord.first().lengthBeats;
        for (const MidiNote& note : chord)
        {
            if (std::abs(note.startBeat - startBeat) > 1e-6 ||
                std::abs(note.lengthBeats - noteLength) > 1e-6)
                return false;
        }

        const double endBeat = std::min(patternLength, startBeat + noteLength);
        const double arpLength = endBeat - startBeat;
        if (arpLength <= 0.0)
            return false;
        const int stepCount = static_cast<int>(std::ceil(arpLength / snap - 1e-9));
        if (stepCount < 1 || stepCount > 4096)
            return false;

        std::sort(chord.begin(), chord.end(), [](const MidiNote& left, const MidiNote& right)
        {
            if (left.pitch != right.pitch) return left.pitch < right.pitch;
            return left.channel < right.channel;
        });
        chord.erase(std::unique(chord.begin(), chord.end(), [](const MidiNote& left, const MidiNote& right)
        {
            return left.pitch == right.pitch;
        }), chord.end());
        if (chord.size() < 2)
            return false;

        direction = std::clamp(direction, 0, 2);
        octaveRange = std::clamp(octaveRange, 1, 4);
        gatePercent = std::clamp(gatePercent, 10, 100);
        velocityPattern = std::clamp(velocityPattern, 0, 4);
        QVector<MidiNote> pitchOrder;
        for (int octave = 0; octave < octaveRange; ++octave)
        {
            for (const MidiNote& note : chord)
            {
                MidiNote octaveNote = note;
                octaveNote.pitch += octave * 12;
                if (octaveNote.pitch <= 127)
                    pitchOrder.push_back(octaveNote);
            }
        }
        if (pitchOrder.size() < 2)
            return false;

        QVector<MidiNote> sequence;
        if (direction == 1)
        {
            for (auto it = pitchOrder.crbegin(); it != pitchOrder.crend(); ++it)
                sequence.push_back(*it);
        }
        else
        {
            sequence = pitchOrder;
            if (direction == 2)
            {
                for (int index = pitchOrder.size() - 2; index > 0; --index)
                    sequence.push_back(pitchOrder[index]);
            }
        }

        const QVector<MidiNote> before = notes;
        quint64 id = nextId();
        notes.erase(std::remove_if(notes.begin(), notes.end(), [this](const MidiNote& note)
        {
            return selected.contains(note.id);
        }), notes.end());
        selected.clear();
        for (int step = 0; step < stepCount; ++step)
        {
            const double noteStart = startBeat + step * snap;
            if (noteStart >= endBeat)
                break;
            MidiNote arpNote = sequence[step % sequence.size()];
            arpNote.id = id++;
            arpNote.startBeat = noteStart;
            arpNote.lengthBeats = std::min(snap * gatePercent / 100.0, endBeat - noteStart);
            double velocityScale = 1.0;
            if (stepCount > 1 && velocityPattern == 1)
            {
                const double progress = static_cast<double>(step) / (stepCount - 1);
                velocityScale = 0.35 + 0.65 * progress;
            }
            else if (stepCount > 1 && velocityPattern == 2)
            {
                const double progress = static_cast<double>(step) / (stepCount - 1);
                velocityScale = 1.0 - 0.65 * progress;
            }
            else if (velocityPattern == 3)
            {
                velocityScale = step % 2 == 0 ? 1.0 : 0.55;
            }
            else if (velocityPattern == 4)
            {
                velocityScale = step % 3 == 0 ? 1.0 : 0.55;
            }
            arpNote.velocity = std::clamp(
                static_cast<int>(std::lround(arpNote.velocity * velocityScale)), 1, 127);
            notes.push_back(arpNote);
            selected.insert(arpNote.id);
        }

        std::sort(notes.begin(), notes.end(), [](const MidiNote& left, const MidiNote& right)
        {
            if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
            return left.pitch < right.pitch;
        });
        commitIfChanged(before);
        update();
        return notes != before;
    }

    bool strumSelectedChord()
    {
        QVector<int> selectedIndices;
        for (int index = 0; index < notes.size(); ++index)
        {
            if (selected.contains(notes[index].id))
                selectedIndices.push_back(index);
        }
        if (selectedIndices.size() < 2)
            return false;

        const double startBeat = notes[selectedIndices.first()].startBeat;
        for (const int index : selectedIndices)
        {
            if (std::abs(notes[index].startBeat - startBeat) > 1e-6)
                return false;
        }
        std::sort(selectedIndices.begin(), selectedIndices.end(), [this](int left, int right)
        {
            if (notes[left].pitch != notes[right].pitch)
                return notes[left].pitch < notes[right].pitch;
            return notes[left].channel < notes[right].channel;
        });

        const QVector<MidiNote> before = notes;
        const double stagger = std::max(1.0 / 64.0, snap * 0.25);
        for (int rank = 0; rank < selectedIndices.size(); ++rank)
        {
            MidiNote& note = notes[selectedIndices[rank]];
            note.startBeat = std::clamp(startBeat + rank * stagger, 0.0,
                                        std::max(0.0, patternLength - note.lengthBeats));
        }
        std::sort(notes.begin(), notes.end(), [](const MidiNote& left, const MidiNote& right)
        {
            if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
            return left.pitch < right.pitch;
        });
        commitIfChanged(before);
        update();
        return notes != before;
    }

    void editSelectedNoteProperties()
    {
        QVector<int> selectedIndices;
        for (int index = 0; index < notes.size(); ++index)
        {
            if (selected.contains(notes[index].id))
                selectedIndices.push_back(index);
        }
        if (selectedIndices.isEmpty())
            return;
        if (selectedIndices.size() == 1)
        {
            editNoteProperties(selectedIndices.first());
            return;
        }
        editMultipleNoteProperties(selectedIndices);
    }

    void setPixelsPerBeat(int pixels)
    {
        pixelsPerBeat = std::clamp(pixels, 36, 180);
        updateCanvasSize();
    }

    void setPlayheadBeat(double beat)
    {
        playheadBeat = beat;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#12161c")));

        const int gridTop = rulerHeight;
        const int gridBottom = gridTop + pitchCount * rowHeight;
        const int velocityTop = gridBottom;
        const int pitchBendTop = velocityTop + velocityHeight;
        const int contentBottom = pitchBendTop + pitchBendHeight;
        const int gridWidth = static_cast<int>(std::ceil(patternLength * pixelsPerBeat));

        painter.fillRect(QRect(sidebarWidth, 0, gridWidth, rulerHeight), QColor(QStringLiteral("#1c222a")));
        painter.fillRect(QRect(0, 0, sidebarWidth, rulerHeight), QColor(QStringLiteral("#202731")));
        painter.fillRect(QRect(0, gridTop, sidebarWidth, pitchCount * rowHeight), QColor(QStringLiteral("#303844")));
        painter.fillRect(QRect(sidebarWidth, velocityTop, gridWidth, velocityHeight), QColor(QStringLiteral("#171c23")));
        painter.fillRect(QRect(0, velocityTop, sidebarWidth, velocityHeight), QColor(QStringLiteral("#202731")));
        painter.fillRect(QRect(sidebarWidth, pitchBendTop, gridWidth, pitchBendHeight), QColor(QStringLiteral("#171c23")));
        painter.fillRect(QRect(0, pitchBendTop, sidebarWidth, pitchBendHeight), QColor(QStringLiteral("#202731")));

        for (int row = 0; row < pitchCount; ++row)
        {
            const int pitch = highestPitch - row;
            const int y = gridTop + row * rowHeight;
            const bool blackKey = isBlackKey(pitch % 12);
            const bool inScale = isScalePitch(pitch);
            const bool rootNote = pitch % 12 == rootPitchClass;
            const QColor lane = !inScale ? QColor(QStringLiteral("#151a21")) :
                rootNote ? QColor(QStringLiteral("#213438")) :
                blackKey ? QColor(QStringLiteral("#1a2429")) : QColor(QStringLiteral("#1d252c"));
            painter.fillRect(QRect(sidebarWidth, y, gridWidth, rowHeight), lane);
            painter.setPen(QColor(QStringLiteral("#29313b")));
            painter.drawLine(sidebarWidth, y + rowHeight - 1, sidebarWidth + gridWidth, y + rowHeight - 1);

            const QColor key = !inScale ?
                (blackKey ? QColor(QStringLiteral("#282f38")) : QColor(QStringLiteral("#7c8792"))) :
                rootNote ? QColor(QStringLiteral("#63c9bb")) :
                blackKey ? QColor(QStringLiteral("#405052")) : QColor(QStringLiteral("#c4cdd2"));
            painter.fillRect(QRect(0, y, sidebarWidth - (blackKey ? 0 : 1), rowHeight - 1), key);
            if (pitch % 12 == 0)
            {
                painter.setPen(blackKey ? QColor(QStringLiteral("#f2f5f8")) : QColor(QStringLiteral("#202730")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                painter.drawText(QRect(7, y, sidebarWidth - 11, rowHeight - 1), Qt::AlignVCenter | Qt::AlignLeft,
                                 noteLabel(pitch));
            }
        }

        const int totalBeats = static_cast<int>(std::ceil(patternLength));
        for (int beat = 0; beat <= totalBeats; ++beat)
        {
            const int x = sidebarWidth + beat * pixelsPerBeat;
            if (!isBarLineAtBeat(beat))
            {
                painter.setPen(QColor(QStringLiteral("#303944")));
                painter.drawLine(x, rulerHeight, x, contentBottom);
            }
        }
        int bar = 1;
        for (double beat = 0.0; beat <= patternLength;)
        {
            const int x = sidebarWidth + static_cast<int>(std::round(beat * pixelsPerBeat));
            painter.setPen(QColor(QStringLiteral("#56616e")));
            painter.drawLine(x, rulerHeight, x, contentBottom);
            if (beat < patternLength)
            {
                painter.setPen(QColor(QStringLiteral("#e3e9ef")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                painter.drawText(QRect(x + 6, 0,
                                       static_cast<int>(std::round(pixelsPerBeat * meterLengthAt(beat))) - 12,
                                       rulerHeight), Qt::AlignVCenter, QString::number(bar + 1));
            }
            double nextBeat = beat + meterLengthAt(beat);
            for (const TimeSignatureChange& change : localTimeSignatureChanges)
                if (change.beat > beat + 1.0e-9 && change.beat < nextBeat)
                {
                    nextBeat = change.beat;
                    break;
                }
            if (nextBeat <= beat + 1.0e-9)
                break;
            beat = nextBeat;
            ++bar;
        }

        if (ghostNotesEnabled && !ghostNotes.isEmpty())
        {
            painter.save();
            painter.setClipRect(QRect(sidebarWidth, gridTop, gridWidth, pitchCount * rowHeight));
            painter.setBrush(QColor(148, 132, 224, 62));
            painter.setPen(QPen(QColor(176, 163, 244, 150), 1.0));
            for (const MidiNote& note : ghostNotes)
            {
                if (note.pitch < lowestPitch || note.pitch > highestPitch)
                    continue;
                painter.drawRoundedRect(noteBounds(note), 3.0, 3.0);
            }
            painter.restore();
        }

        painter.setPen(QColor(QStringLiteral("#75818e")));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
        painter.drawText(QRect(8, velocityTop + 7, sidebarWidth - 12, 20), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("VELOCITY"));

        const int bendUsableHeight = pitchBendHeight - 20;
        const int bendCenterY = pitchBendTop + 10 + bendUsableHeight / 2;
        painter.setPen(QPen(QColor(QStringLiteral("#35434b")), 1.0, Qt::DashLine));
        painter.drawLine(sidebarWidth, bendCenterY, sidebarWidth + gridWidth, bendCenterY);
        painter.setPen(QColor(QStringLiteral("#75818e")));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
        painter.drawText(QRect(8, pitchBendTop + 4, sidebarWidth - 12, 12),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("PITCH BEND"));
        const QString rangeLabel = pitchBendRangeLabel(pitchBendRangeSemitones);
        painter.drawText(QRect(8, pitchBendTop + 18, sidebarWidth - 12, 9),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("+%1 st").arg(rangeLabel));
        painter.drawText(QRect(8, bendCenterY - 4, sidebarWidth - 12, 9),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));
        painter.drawText(QRect(8, pitchBendTop + pitchBendHeight - 14, sidebarWidth - 12, 9),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("-%1 st").arg(rangeLabel));

        if (!pitchBendLaneEnabled)
        {
            painter.fillRect(QRect(sidebarWidth, pitchBendTop, gridWidth, pitchBendHeight),
                             QColor(QStringLiteral("#141920")));
            painter.setPen(QColor(QStringLiteral("#66717d")));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
            painter.drawText(QRect(sidebarWidth + 10, pitchBendTop + 6,
                                   std::max(0, gridWidth - 20), pitchBendHeight - 12),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             tr("Select a MIDI clip to edit pitch bend."));
        }
        else if (pitchBends.isEmpty())
        {
            painter.setPen(QColor(QStringLiteral("#85919e")));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
            painter.drawText(QRect(sidebarWidth + 10, pitchBendTop + 6,
                                   std::max(0, gridWidth - 20), pitchBendHeight - 12),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             tr("Click to add pitch bend points; drag to shape the curve."));
        }

        for (int channel = 0; channel < 16; ++channel)
        {
            if (!pitchBendLaneEnabled)
                break;
            bool hasPrevious = false;
            QPoint previousPoint;
            for (int index = 0; index < pitchBends.size(); ++index)
            {
                const MidiPitchBend& bend = pitchBends[index];
                if (bend.channel != channel)
                    continue;
                const QPoint currentPoint(
                    sidebarWidth + static_cast<int>(std::round(bend.beat * pixelsPerBeat)),
                    pitchBendY(bend.value, pitchBendTop));
                const bool selectedBend = index == selectedPitchBendIndex;
                const QColor bendColor = QColor(QStringLiteral("#63c9bb"));
                painter.setPen(QPen(QColor(bendColor.red(), bendColor.green(), bendColor.blue(), 115), 1.0));
                painter.drawLine(currentPoint.x(), bendCenterY, currentPoint.x(), currentPoint.y());
                if (hasPrevious)
                {
                    painter.setPen(QPen(bendColor, 1.5));
                    painter.drawLine(previousPoint.x(), previousPoint.y(), currentPoint.x(), previousPoint.y());
                    painter.drawLine(currentPoint.x(), previousPoint.y(), currentPoint.x(), currentPoint.y());
                }
                painter.setPen(QPen(selectedBend ? QColor(QStringLiteral("#d7fff8")) : bendColor,
                                    selectedBend ? 1.5 : 1.0));
                painter.setBrush(selectedBend ? QColor(QStringLiteral("#a7f2e5")) : bendColor);
                painter.drawEllipse(QPointF(currentPoint), selectedBend ? 4.5 : 3.5,
                                    selectedBend ? 4.5 : 3.5);
                previousPoint = currentPoint;
                hasPrevious = true;
            }
        }

        if (notes.isEmpty())
        {
            painter.setPen(QColor(QStringLiteral("#85919e")));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 10));
            painter.drawText(QRect(sidebarWidth + 12, gridTop + 12, gridWidth - 24, 28), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Click and drag on the grid to draw notes. Right-click to delete."));
        }

        for (const MidiNote& note : notes)
        {
            const QRectF bounds = noteBounds(note);
            const bool isSelected = selected.contains(note.id);
            const QColor fill = isSelected ? QColor(QStringLiteral("#4ccfbd")) : QColor(QStringLiteral("#4e9dc1"));
            painter.setPen(QPen(isSelected ? QColor(QStringLiteral("#b9fff3")) : QColor(QStringLiteral("#79c7df")), 1.0));
            painter.setBrush(fill);
            painter.drawRoundedRect(bounds, 3.0, 3.0);

            painter.setPen(isSelected ? QColor(QStringLiteral("#112b2c")) : QColor(QStringLiteral("#10232c")));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
            if (bounds.width() > 48.0)
                painter.drawText(bounds.adjusted(6.0, 0.0, -3.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft,
                                 noteLabel(note.pitch));

            const int velocityX = static_cast<int>(std::round(sidebarWidth +
                (note.startBeat + note.lengthBeats * 0.5) * pixelsPerBeat));
            const int velocityY = velocityTop + velocityHeight - 8 - note.velocity * (velocityHeight - 25) / 127;
            painter.setPen(QPen(isSelected ? QColor(QStringLiteral("#70e0ce")) : QColor(QStringLiteral("#6a8c9b")),
                                isSelected ? 2.0 : 1.0));
            painter.drawLine(velocityX, velocityTop + velocityHeight - 5, velocityX, velocityY);
            painter.setBrush(isSelected ? QColor(QStringLiteral("#70e0ce")) : QColor(QStringLiteral("#6a8c9b")));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(velocityX, velocityY), 3.0, 3.0);
        }

        if (playheadBeat >= 0.0 && playheadBeat <= patternLength)
        {
            const int x = sidebarWidth + static_cast<int>(std::round(playheadBeat * pixelsPerBeat));
            painter.setPen(QPen(QColor(QStringLiteral("#f1a85c")), 2.0));
            painter.drawLine(x, rulerHeight, x, contentBottom);
            painter.setBrush(QColor(QStringLiteral("#f1a85c")));
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(QPolygon{QPoint(x - 5, 0), QPoint(x + 5, 0), QPoint(x, 7)});
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        setFocus(Qt::MouseFocusReason);
        const QPoint point = event->position().toPoint();
        const int pitchBendTop = rulerHeight + pitchCount * rowHeight + velocityHeight;
        const bool inPitchBendLane = point.y() >= pitchBendTop &&
                                     point.y() < pitchBendTop + pitchBendHeight;
        if (!inPitchBendLane)
            selectedPitchBendIndex = -1;

        if (event->button() == Qt::LeftButton && point.x() < sidebarWidth &&
            point.y() >= rulerHeight && point.y() < rulerHeight + pitchCount * rowHeight)
        {
            emit owner->previewRequested(pitchAt(point.y()));
            return;
        }

        if (event->button() == Qt::RightButton)
        {
            if (inPitchBendLane)
            {
                if (!pitchBendLaneEnabled)
                    return;
                const int hit = pitchBendAt(point, pitchBendTop);
                if (hit >= 0)
                {
                    beforePitchBends = pitchBends;
                    pitchBends.removeAt(hit);
                    selectedPitchBendIndex = -1;
                    commitPitchBendsIfChanged(beforePitchBends);
                    update();
                }
                return;
            }
            const int hit = noteAt(point);
            if (hit >= 0)
            {
                const QVector<MidiNote> before = notes;
                selected.remove(notes[hit].id);
                notes.removeAt(hit);
                commitIfChanged(before);
                update();
            }
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;

        if (inPitchBendLane)
        {
            if (!pitchBendLaneEnabled)
                return;
            if (point.x() < sidebarWidth)
                return;
            beforePitchBends = pitchBends;
            selectedPitchBendIndex = pitchBendAt(point, pitchBendTop);
            if (selectedPitchBendIndex < 0)
            {
                MidiPitchBend bend;
                bend.beat = std::clamp(snapNearest(beatAt(point.x())), 0.0, patternLength);
                bend.channel = preferredPitchBendChannel(bend.beat);
                bend.value = pitchBendValueAt(point.y(), pitchBendTop);
                pitchBends.push_back(bend);
                selectedPitchBendIndex = pitchBends.size() - 1;
            }
            draggedPitchBendIndex = selectedPitchBendIndex;
            dragMode = DragMode::PitchBend;
            update();
            return;
        }

        const int velocityTop = rulerHeight + pitchCount * rowHeight;
        if (point.y() >= velocityTop && point.y() < velocityTop + velocityHeight)
        {
            const int hit = noteNearVelocity(point.x());
            if (hit < 0)
                return;
            beforeEdit = notes;
            if (!selected.contains(notes[hit].id))
                selected = {notes[hit].id};
            draggedNoteId = notes[hit].id;
            dragMode = DragMode::Velocity;
            setVelocityAt(point.y());
            update();
            return;
        }
        if (point.y() < rulerHeight || point.y() >= velocityTop || point.x() < sidebarWidth)
            return;

        const int hit = noteAt(point);
        beforeEdit = notes;
        dragStart = point;
        originalNotes = notes;
        if (hit >= 0)
        {
            const MidiNote target = notes[hit];
            if (event->modifiers().testFlag(Qt::ShiftModifier))
            {
                selected.insert(target.id);
            }
            else if (!selected.contains(target.id))
            {
                selected = {target.id};
            }
            draggedNoteId = target.id;
            const QRectF bounds = noteBounds(target);
            dragMode = point.x() >= bounds.right() - 9.0 ? DragMode::Resize : DragMode::Move;
        }
        else
        {
            selected.clear();
            const double minimumLength = std::min(snapIntervalAt(beatAt(point.x())), patternLength);
            const double startBeat = std::min(snapDown(beatAt(point.x())),
                                              std::max(0.0, patternLength - minimumLength));
            MidiNote note;
            note.id = nextId();
            note.pitch = pitchAt(point.y());
            note.velocity = 96;
            note.startBeat = startBeat;
            note.lengthBeats = std::min(snapIntervalAt(startBeat),
                                        std::max(1.0 / 64.0, patternLength - startBeat));
            notes.push_back(note);
            selected.insert(note.id);
            draggedNoteId = note.id;
            dragMode = DragMode::Create;
            emit owner->previewRequested(note.pitch);
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (dragMode == DragMode::None)
            return;
        const QPoint point = event->position().toPoint();

        if (dragMode == DragMode::Velocity)
        {
            setVelocityAt(point.y());
            update();
            return;
        }

        if (dragMode == DragMode::PitchBend)
        {
            setPitchBendAt(point, rulerHeight + pitchCount * rowHeight + velocityHeight);
            update();
            return;
        }

        if (dragMode == DragMode::Move)
        {
            const double rawDeltaBeat = (point.x() - dragStart.x()) / static_cast<double>(pixelsPerBeat);
            const int deltaPitch = static_cast<int>(std::round((dragStart.y() - point.y()) / static_cast<double>(rowHeight)));
            notes = originalNotes;
            for (MidiNote& note : notes)
            {
                if (!selected.contains(note.id))
                    continue;
                const double targetBeat = snapToBar ? snapNearest(note.startBeat + rawDeltaBeat)
                    : note.startBeat + std::round(rawDeltaBeat / snap) * snap;
                note.startBeat = std::clamp(targetBeat, 0.0,
                                            std::max(0.0, patternLength - note.lengthBeats));
                note.pitch = std::clamp(note.pitch + deltaPitch, 0, 127);
            }
        }
        else if (dragMode == DragMode::Resize || dragMode == DragMode::Create)
        {
            const double endBeat = snapNearest(beatAt(point.x()));
            const double minimumLength = std::min(snapIntervalAt(beatAt(point.x())), patternLength);
            for (MidiNote& note : notes)
            {
                if (note.id != draggedNoteId)
                    continue;
                note.lengthBeats = std::clamp(endBeat - note.startBeat, minimumLength,
                                              std::max(minimumLength, patternLength - note.startBeat));
                break;
            }
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && dragMode != DragMode::None)
        {
            const bool editedPitchBend = dragMode == DragMode::PitchBend;
            dragMode = DragMode::None;
            if (editedPitchBend)
            {
                MidiPitchBend selectedBend;
                const bool hadSelectedBend = draggedPitchBendIndex >= 0 &&
                                             draggedPitchBendIndex < pitchBends.size();
                if (hadSelectedBend)
                    selectedBend = pitchBends[draggedPitchBendIndex];
                std::stable_sort(pitchBends.begin(), pitchBends.end(),
                    [](const MidiPitchBend& left, const MidiPitchBend& right)
                    {
                        return left.beat < right.beat;
                    });
                selectedPitchBendIndex = -1;
                if (hadSelectedBend)
                {
                    for (int index = 0; index < pitchBends.size(); ++index)
                    {
                        if (pitchBends[index] == selectedBend)
                        {
                            selectedPitchBendIndex = index;
                            break;
                        }
                    }
                }
                draggedPitchBendIndex = -1;
                commitPitchBendsIfChanged(beforePitchBends);
            }
            else
            {
                commitIfChanged(beforeEdit);
            }
            update();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            const int hit = noteAt(event->position().toPoint());
            if (hit >= 0)
            {
                selected = {notes[hit].id};
                editNoteProperties(hit);
                return;
            }
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
            selectedPitchBendIndex >= 0 && selectedPitchBendIndex < pitchBends.size())
        {
            beforePitchBends = pitchBends;
            pitchBends.removeAt(selectedPitchBendIndex);
            selectedPitchBendIndex = -1;
            commitPitchBendsIfChanged(beforePitchBends);
            update();
            return;
        }
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && !selected.isEmpty())
        {
            const QVector<MidiNote> before = notes;
            notes.erase(std::remove_if(notes.begin(), notes.end(), [this](const MidiNote& note)
            {
                return selected.contains(note.id);
            }), notes.end());
            selected.clear();
            commitIfChanged(before);
            update();
            return;
        }
        if (event->key() == Qt::Key_Escape)
        {
            selected.clear();
            selectedPitchBendIndex = -1;
            update();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    enum class DragMode { None, Create, Move, Resize, Velocity, PitchBend };

    void updatePitchBendToolTip()
    {
        setToolTip(tr("Pitch bend: click to add a point, drag to shape it, and right-click or press Delete to remove it. Range: ±%1 semitones.")
                       .arg(pitchBendRangeLabel(pitchBendRangeSemitones)));
    }

    void editNoteProperties(int index)
    {
        if (index < 0 || index >= notes.size())
            return;

        const MidiNote initialNote = notes[index];
        QDialog dialog(this);
        dialog.setWindowTitle(tr("MIDI note properties"));
        auto* form = new QFormLayout(&dialog);

        auto* pitchControl = new QSpinBox(&dialog);
        pitchControl->setRange(0, 127);
        pitchControl->setValue(initialNote.pitch);
        form->addRow(tr("Pitch"), pitchControl);

        auto* startControl = new QDoubleSpinBox(&dialog);
        startControl->setRange(0.0, patternLength);
        startControl->setDecimals(4);
        startControl->setSingleStep(snap);
        startControl->setSuffix(tr(" beats"));
        startControl->setValue(initialNote.startBeat);
        form->addRow(tr("Start"), startControl);

        auto* lengthControl = new QDoubleSpinBox(&dialog);
        lengthControl->setRange(1.0 / 64.0, std::max(1.0 / 64.0, patternLength));
        lengthControl->setDecimals(4);
        lengthControl->setSingleStep(snap);
        lengthControl->setSuffix(tr(" beats"));
        lengthControl->setValue(initialNote.lengthBeats);
        form->addRow(tr("Length"), lengthControl);

        auto* velocityControl = new QSpinBox(&dialog);
        velocityControl->setRange(1, 127);
        velocityControl->setValue(initialNote.velocity);
        form->addRow(tr("Velocity"), velocityControl);

        auto* channelControl = new QComboBox(&dialog);
        channelControl->addItem(tr("Track default"), -1);
        for (int channel = 0; channel < 16; ++channel)
            channelControl->addItem(QString::number(channel + 1), channel);
        const int channelIndex = channelControl->findData(initialNote.channel);
        channelControl->setCurrentIndex(channelIndex < 0 ? 0 : channelIndex);
        form->addRow(tr("MIDI channel"), channelControl);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;

        const auto currentNote = std::find_if(notes.begin(), notes.end(),
            [&initialNote](const MidiNote& note) { return note.id == initialNote.id; });
        if (currentNote == notes.end())
            return;

        const QVector<MidiNote> before = notes;
        currentNote->pitch = pitchControl->value();
        currentNote->lengthBeats = lengthControl->value();
        currentNote->startBeat = std::clamp(startControl->value(), 0.0,
                                             std::max(0.0, patternLength - currentNote->lengthBeats));
        currentNote->velocity = velocityControl->value();
        currentNote->channel = channelControl->currentData().toInt();
        selected = {currentNote->id};
        commitIfChanged(before);
        update();
    }

    void editMultipleNoteProperties(const QVector<int>& selectedIndices)
    {
        if (selectedIndices.size() < 2)
            return;

        int velocityTotal = 0;
        for (const int index : selectedIndices)
            velocityTotal += notes[index].velocity;

        QDialog dialog(this);
        dialog.setWindowTitle(tr("MIDI note properties · %1 selected").arg(selectedIndices.size()));
        auto* form = new QFormLayout(&dialog);

        auto* transposeCheck = new QCheckBox(tr("Apply"), &dialog);
        auto* transposeControl = new QSpinBox(&dialog);
        transposeControl->setRange(-127, 127);
        transposeControl->setSuffix(tr(" semitones"));
        transposeControl->setEnabled(false);
        connect(transposeCheck, &QCheckBox::toggled, transposeControl, &QWidget::setEnabled);
        form->addRow(tr("Transpose pitches"), transposeCheck);
        form->addRow(tr("Pitch shift"), transposeControl);

        auto* startCheck = new QCheckBox(tr("Apply"), &dialog);
        auto* startControl = new QDoubleSpinBox(&dialog);
        startControl->setRange(-patternLength, patternLength);
        startControl->setDecimals(4);
        startControl->setSingleStep(snap);
        startControl->setSuffix(tr(" beats"));
        startControl->setEnabled(false);
        connect(startCheck, &QCheckBox::toggled, startControl, &QWidget::setEnabled);
        form->addRow(tr("Shift note starts"), startCheck);
        form->addRow(tr("Timing shift"), startControl);

        auto* lengthCheck = new QCheckBox(tr("Apply"), &dialog);
        auto* lengthControl = new QDoubleSpinBox(&dialog);
        lengthControl->setRange(1.0 / 64.0, std::max(1.0 / 64.0, patternLength));
        lengthControl->setDecimals(4);
        lengthControl->setSingleStep(snap);
        lengthControl->setSuffix(tr(" beats"));
        lengthControl->setValue(notes[selectedIndices.first()].lengthBeats);
        lengthControl->setEnabled(false);
        connect(lengthCheck, &QCheckBox::toggled, lengthControl, &QWidget::setEnabled);
        form->addRow(tr("Set note length"), lengthCheck);
        form->addRow(tr("New length"), lengthControl);

        auto* velocityCheck = new QCheckBox(tr("Apply"), &dialog);
        auto* velocityControl = new QSpinBox(&dialog);
        velocityControl->setRange(1, 127);
        velocityControl->setValue(static_cast<int>(std::lround(
            static_cast<double>(velocityTotal) / selectedIndices.size())));
        velocityControl->setEnabled(false);
        connect(velocityCheck, &QCheckBox::toggled, velocityControl, &QWidget::setEnabled);
        form->addRow(tr("Set velocity"), velocityCheck);
        form->addRow(tr("New velocity"), velocityControl);

        auto* channelCheck = new QCheckBox(tr("Apply"), &dialog);
        auto* channelControl = new QComboBox(&dialog);
        channelControl->addItem(tr("Keep each note's channel"), -2);
        channelControl->addItem(tr("Track default"), -1);
        for (int channel = 0; channel < 16; ++channel)
            channelControl->addItem(QString::number(channel + 1), channel);
        channelControl->setEnabled(false);
        connect(channelCheck, &QCheckBox::toggled, channelControl, &QWidget::setEnabled);
        form->addRow(tr("Set MIDI channel"), channelCheck);
        form->addRow(tr("New channel"), channelControl);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;

        const QVector<MidiNote> before = notes;
        const int pitchShift = transposeCheck->isChecked() ? transposeControl->value() : 0;
        const double startShift = startCheck->isChecked() ? startControl->value() : 0.0;
        const int selectedChannel = channelControl->currentData().toInt();
        for (MidiNote& note : notes)
        {
            if (!selected.contains(note.id))
                continue;
            note.pitch = std::clamp(note.pitch + pitchShift, 0, 127);
            if (startCheck->isChecked())
                note.startBeat = std::clamp(note.startBeat + startShift, 0.0,
                    std::max(0.0, patternLength - note.lengthBeats));
            if (lengthCheck->isChecked())
            {
                const double maxLength = std::max(1.0 / 64.0, patternLength - note.startBeat);
                note.lengthBeats = std::clamp(lengthControl->value(), 1.0 / 64.0, maxLength);
            }
            if (velocityCheck->isChecked())
                note.velocity = velocityControl->value();
            if (channelCheck->isChecked() && selectedChannel != -2)
                note.channel = selectedChannel;
        }
        std::sort(notes.begin(), notes.end(), [](const MidiNote& left, const MidiNote& right)
        {
            if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
            return left.pitch < right.pitch;
        });
        commitIfChanged(before);
        update();
    }

    void updateCanvasSize()
    {
        const int width = sidebarWidth + static_cast<int>(std::ceil(patternLength * pixelsPerBeat)) + 40;
        const int height = rulerHeight + pitchCount * rowHeight + velocityHeight + pitchBendHeight;
        setMinimumSize(width, height);
        resize(width, height);
        update();
    }

    double beatAt(int x) const
    {
        return std::clamp((x - sidebarWidth) / static_cast<double>(pixelsPerBeat), 0.0, patternLength);
    }

    int pitchAt(int y) const
    {
        const int row = std::clamp((y - rulerHeight) / rowHeight, 0, pitchCount - 1);
        const int pitch = std::clamp(highestPitch - row, 0, 127);
        if (!snapNewNotesToScale || isScalePitch(pitch))
            return pitch;

        int nearestPitch = pitch;
        int nearestDistance = highestPitch + 1;
        for (int candidate = 0; candidate <= highestPitch; ++candidate)
        {
            if (!isScalePitch(candidate))
                continue;
            const int distance = std::abs(candidate - pitch);
            if (distance < nearestDistance)
            {
                nearestPitch = candidate;
                nearestDistance = distance;
            }
        }
        return nearestPitch;
    }

    bool isScalePitch(int pitch) const
    {
        const int pitchClass = ((pitch - rootPitchClass) % 12 + 12) % 12;
        return scaleIntervals.contains(pitchClass);
    }

    double snapDown(double beat) const
    {
        if (snapToBar)
        {
            const auto [barStart, barLength] = barAtBeat(beat);
            const double step = barLength / barSnapDivisions;
            return barStart + std::floor((beat - barStart) / step) * step;
        }
        return std::floor(beat / snap) * snap;
    }

    double snapNearest(double beat) const
    {
        if (snapToBar)
        {
            const auto [barStart, barLength] = barAtBeat(beat);
            const double step = barLength / barSnapDivisions;
            return barStart + std::round((beat - barStart) / step) * step;
        }
        return std::round(beat / snap) * snap;
    }

    double snapIntervalAt(double beat) const
    {
        if (!snapToBar)
            return snap;
        return meterLengthAt(beat) / barSnapDivisions;
    }

    std::pair<double, double> barAtBeat(double beat) const
    {
        double segmentStart = 0.0;
        double barLength = meterLengthAt(0.0);
        for (const TimeSignatureChange& change : localTimeSignatureChanges)
        {
            if (change.beat <= 1.0e-9)
                continue;
            if (change.beat > beat + 1.0e-9)
                break;
            segmentStart = change.beat;
            barLength = static_cast<double>(change.beatsPerBar) * 4.0 / change.beatUnit;
        }
        const double barStart = segmentStart +
            std::floor(std::max(0.0, beat - segmentStart) / barLength) * barLength;
        return {barStart, barLength};
    }

    QRectF noteBounds(const MidiNote& note) const
    {
        const int row = highestPitch - note.pitch;
        const qreal x = sidebarWidth + note.startBeat * pixelsPerBeat + 1.0;
        const qreal y = rulerHeight + row * rowHeight + 1.0;
        return QRectF(x, y, std::max(3.0, note.lengthBeats * pixelsPerBeat - 2.0), rowHeight - 2.0);
    }

    int noteAt(const QPoint& point) const
    {
        for (int index = notes.size() - 1; index >= 0; --index)
        {
            if (noteBounds(notes[index]).contains(point))
                return index;
        }
        return -1;
    }

    int noteNearVelocity(int x) const
    {
        int nearest = -1;
        int nearestDistance = 8;
        for (int index = 0; index < notes.size(); ++index)
        {
            const int noteX = static_cast<int>(std::round(sidebarWidth +
                (notes[index].startBeat + notes[index].lengthBeats * 0.5) * pixelsPerBeat));
            const int distance = std::abs(noteX - x);
            if (distance < nearestDistance)
            {
                nearest = index;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    void setVelocityAt(int y)
    {
        const int velocityTop = rulerHeight + pitchCount * rowHeight;
        const int usableHeight = velocityHeight - 25;
        const int value = std::clamp((velocityTop + velocityHeight - 8 - y) * 127 / usableHeight, 1, 127);
        for (MidiNote& note : notes)
        {
            if (note.id == draggedNoteId)
            {
                note.velocity = value;
                break;
            }
        }
    }

    int pitchBendY(int value, int laneTop) const
    {
        const int usableHeight = pitchBendHeight - 20;
        const int boundedValue = std::clamp(value, 0, 16383);
        return laneTop + 10 + (16383 - boundedValue) * usableHeight / 16383;
    }

    int pitchBendValueAt(int y, int laneTop) const
    {
        const int usableHeight = pitchBendHeight - 20;
        const int offset = std::clamp(y - (laneTop + 10), 0, usableHeight);
        return std::clamp(static_cast<int>(std::llround(
            static_cast<double>(usableHeight - offset) * 16383.0 / usableHeight)), 0, 16383);
    }

    int pitchBendAt(const QPoint& point, int laneTop) const
    {
        int nearest = -1;
        int nearestDistance = 9 * 9 + 1;
        for (int index = 0; index < pitchBends.size(); ++index)
        {
            const QPoint bendPoint(sidebarWidth + static_cast<int>(std::round(
                pitchBends[index].beat * pixelsPerBeat)),
                pitchBendY(pitchBends[index].value, laneTop));
            const int dx = bendPoint.x() - point.x();
            const int dy = bendPoint.y() - point.y();
            const int distance = dx * dx + dy * dy;
            if (distance <= 9 * 9 && distance < nearestDistance)
            {
                nearest = index;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    int preferredPitchBendChannel(double beat) const
    {
        int nearestChannel = -1;
        double nearestDistance = std::numeric_limits<double>::max();
        for (const MidiNote& note : notes)
        {
            if (note.channel < 0)
                continue;
            if (beat >= note.startBeat && beat <= note.startBeat + note.lengthBeats)
                return note.channel;
            const double distance = std::min(std::abs(beat - note.startBeat),
                std::abs(beat - note.startBeat - note.lengthBeats));
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestChannel = note.channel;
            }
        }
        if (nearestChannel >= 0)
            return nearestChannel;
        if (!pitchBends.isEmpty())
            return pitchBends.front().channel;
        return defaultPitchBendChannel;
    }

    void setPitchBendAt(const QPoint& point, int laneTop)
    {
        if (draggedPitchBendIndex < 0 || draggedPitchBendIndex >= pitchBends.size())
            return;
        MidiPitchBend& bend = pitchBends[draggedPitchBendIndex];
        bend.beat = std::clamp(snapNearest(beatAt(point.x())), 0.0, patternLength);
        bend.value = pitchBendValueAt(point.y(), laneTop);
    }

    quint64 nextId() const
    {
        quint64 id = 1;
        for (const MidiNote& note : notes)
            id = std::max(id, note.id + 1);
        return id;
    }

    void commitIfChanged(const QVector<MidiNote>& before)
    {
        if (before != notes)
            emit owner->notesEdited(before, notes);
    }

    void commitPitchBendsIfChanged(const QVector<MidiPitchBend>& before)
    {
        if (before != pitchBends)
            emit owner->pitchBendsEdited(before, pitchBends);
    }

    MidiEditor* owner;
    QVector<MidiNote> notes;
    QVector<MidiPitchBend> pitchBends;
    QVector<MidiNote> ghostNotes;
    QVector<MidiNote> beforeEdit;
    QVector<MidiNote> originalNotes;
    QVector<MidiPitchBend> beforePitchBends;
    QVector<int> scaleIntervals{0, 2, 4, 5, 7, 9, 11};
    QSet<quint64> selected;
    QPoint dragStart;
    DragMode dragMode = DragMode::None;
    quint64 draggedNoteId = 0;
    int selectedPitchBendIndex = -1;
    int draggedPitchBendIndex = -1;
    int defaultPitchBendChannel = 0;
    double pitchBendRangeSemitones = 2.0;
    bool pitchBendLaneEnabled = false;
    double patternLength = 16.0;
    double quarterNotesPerBar = 4.0;
    QVector<TimeSignatureChange> localTimeSignatureChanges{{0.0, 4, 4}};
    double snap = 0.25;
    int barSnapDivisions = 1;
    bool snapToBar = false;
    double playheadBeat = -1.0;
    int pixelsPerBeat = 84;
    int rootPitchClass = 0;
    bool snapNewNotesToScale = false;
    bool ghostNotesEnabled = true;

    double meterLengthAt(double beat) const
    {
        const auto next = std::upper_bound(localTimeSignatureChanges.cbegin(),
            localTimeSignatureChanges.cend(), beat,
            [](double value, const TimeSignatureChange& change) { return value < change.beat; });
        const TimeSignatureChange& meter = next == localTimeSignatureChanges.cbegin()
            ? localTimeSignatureChanges.first() : *(next - 1);
        return static_cast<double>(meter.beatsPerBar) * 4.0 / meter.beatUnit;
    }

    bool isBarLineAtBeat(double beat) const
    {
        if (std::abs(beat) < 0.02)
            return true;
        double segmentStart = 0.0;
        double segmentLength = meterLengthAt(0.0);
        for (const TimeSignatureChange& change : localTimeSignatureChanges)
        {
            if (change.beat <= 1.0e-9)
                continue;
            if (change.beat > beat + 1.0e-9)
                break;
            segmentStart = change.beat;
            segmentLength = static_cast<double>(change.beatsPerBar) * 4.0 / change.beatUnit;
        }
        const double offset = std::fmod(std::max(0.0, beat - segmentStart), segmentLength);
        return std::min(offset, segmentLength - offset) < 0.02;
    }
};

MidiEditor::MidiEditor(QWidget* parent) : QScrollArea(parent)
{
    setObjectName(QStringLiteral("midiEditor"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    canvas = new Canvas(this);
    setWidget(canvas);
}

void MidiEditor::setNotes(const QVector<MidiNote>& notes)
{
    canvas->setNotes(notes);
}

void MidiEditor::setPitchBends(const QVector<MidiPitchBend>& pitchBends)
{
    canvas->setPitchBends(pitchBends);
}

void MidiEditor::setDefaultPitchBendChannel(int channel)
{
    canvas->setDefaultPitchBendChannel(channel);
}

void MidiEditor::setPitchBendLaneEnabled(bool enabled)
{
    canvas->setPitchBendLaneEnabled(enabled);
}

void MidiEditor::setPitchBendRange(double semitones)
{
    canvas->setPitchBendRange(semitones);
}

void MidiEditor::setPatternLengthBeats(double beats)
{
    canvas->setLengthBeats(beats);
}

void MidiEditor::setTimeSignature(int beatsPerBar, int beatUnit)
{
    canvas->setTimeSignature(beatsPerBar, beatUnit);
}

void MidiEditor::setTimeSignatureMap(const QVector<TimeSignatureChange>& changes, double clipStartBeat)
{
    canvas->setTimeSignatureMap(changes, clipStartBeat);
}

void MidiEditor::setSnap(double beatStep)
{
    canvas->setSnap(beatStep);
}

void MidiEditor::setScale(int rootPitchClass, const QVector<int>& intervals, bool snapNewNotes)
{
    canvas->setScale(rootPitchClass, intervals, snapNewNotes);
}

void MidiEditor::setGhostNotes(const QVector<MidiNote>& notes)
{
    canvas->setGhostNotes(notes);
}

void MidiEditor::setGhostNotesEnabled(bool enabled)
{
    canvas->setGhostNotesEnabled(enabled);
}

void MidiEditor::quantizeNotesToGrid()
{
    canvas->quantizeNotesToGrid();
}

bool MidiEditor::addChordToSelection(const QVector<int>& semitoneIntervals)
{
    return canvas->addChordToSelection(semitoneIntervals);
}

bool MidiEditor::arpeggiateSelectedChord(int direction, int octaveRange, int gatePercent,
                                         int velocityPattern)
{
    return canvas->arpeggiateSelectedChord(direction, octaveRange, gatePercent, velocityPattern);
}

bool MidiEditor::strumSelectedChord()
{
    return canvas->strumSelectedChord();
}

void MidiEditor::editSelectedNoteProperties()
{
    canvas->editSelectedNoteProperties();
}

void MidiEditor::setPixelsPerBeat(int pixels)
{
    canvas->setPixelsPerBeat(pixels);
}

void MidiEditor::setPlayheadBeat(double beat)
{
    canvas->setPlayheadBeat(beat);
}
