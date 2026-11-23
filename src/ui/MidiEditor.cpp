#include "MidiEditor.h"

#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int sidebarWidth = 58;
constexpr int rulerHeight = 30;
constexpr int rowHeight = 18;
constexpr int velocityHeight = 100;
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
}

class MidiEditor::Canvas final : public QWidget
{
public:
    explicit Canvas(MidiEditor* owner) : QWidget(owner), owner(owner)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
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

    void setLengthBeats(double beats)
    {
        patternLength = std::max(1.0, beats);
        updateCanvasSize();
    }

    void setSnap(double beatStep)
    {
        snap = std::clamp(beatStep, 1.0 / 64.0, 4.0);
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
        const int gridWidth = static_cast<int>(std::ceil(patternLength * pixelsPerBeat));

        painter.fillRect(QRect(sidebarWidth, 0, gridWidth, rulerHeight), QColor(QStringLiteral("#1c222a")));
        painter.fillRect(QRect(0, 0, sidebarWidth, rulerHeight), QColor(QStringLiteral("#202731")));
        painter.fillRect(QRect(0, gridTop, sidebarWidth, pitchCount * rowHeight), QColor(QStringLiteral("#303844")));
        painter.fillRect(QRect(sidebarWidth, velocityTop, gridWidth, velocityHeight), QColor(QStringLiteral("#171c23")));

        for (int row = 0; row < pitchCount; ++row)
        {
            const int pitch = highestPitch - row;
            const int y = gridTop + row * rowHeight;
            const bool blackKey = isBlackKey(pitch % 12);
            const QColor lane = blackKey ? QColor(QStringLiteral("#171c23")) : QColor(QStringLiteral("#1b2129"));
            painter.fillRect(QRect(sidebarWidth, y, gridWidth, rowHeight), lane);
            painter.setPen(QColor(QStringLiteral("#29313b")));
            painter.drawLine(sidebarWidth, y + rowHeight - 1, sidebarWidth + gridWidth, y + rowHeight - 1);

            const QColor key = blackKey ? QColor(QStringLiteral("#303944")) : QColor(QStringLiteral("#b9c1cc"));
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
            const bool barLine = beat % 4 == 0;
            painter.setPen(barLine ? QColor(QStringLiteral("#56616e")) : QColor(QStringLiteral("#303944")));
            painter.drawLine(x, rulerHeight, x, velocityTop + velocityHeight);
            if (barLine && beat < patternLength)
            {
                painter.setPen(QColor(QStringLiteral("#e3e9ef")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                painter.drawText(QRect(x + 6, 0, pixelsPerBeat * 4 - 12, rulerHeight), Qt::AlignVCenter,
                                 QString::number(beat / 4 + 1));
            }
        }

        painter.setPen(QColor(QStringLiteral("#75818e")));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
        painter.drawText(QRect(8, velocityTop + 7, sidebarWidth - 12, 20), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("VELOCITY"));

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
            painter.drawLine(x, rulerHeight, x, velocityTop + velocityHeight);
            painter.setBrush(QColor(QStringLiteral("#f1a85c")));
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(QPolygon{QPoint(x - 5, 0), QPoint(x + 5, 0), QPoint(x, 7)});
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        setFocus(Qt::MouseFocusReason);
        const QPoint point = event->position().toPoint();
        if (event->button() == Qt::LeftButton && point.x() < sidebarWidth &&
            point.y() >= rulerHeight && point.y() < rulerHeight + pitchCount * rowHeight)
        {
            emit owner->previewRequested(pitchAt(point.y()));
            return;
        }

        if (event->button() == Qt::RightButton)
        {
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
            const double minimumLength = std::min(snap, patternLength);
            const double startBeat = std::min(snapDown(beatAt(point.x())),
                                              std::max(0.0, patternLength - minimumLength));
            MidiNote note;
            note.id = nextId();
            note.pitch = pitchAt(point.y());
            note.velocity = 96;
            note.startBeat = startBeat;
            note.lengthBeats = std::min(snap, std::max(1.0 / 64.0, patternLength - startBeat));
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

        if (dragMode == DragMode::Move)
        {
            const double deltaBeat = std::round(((point.x() - dragStart.x()) / static_cast<double>(pixelsPerBeat)) / snap) * snap;
            const int deltaPitch = static_cast<int>(std::round((dragStart.y() - point.y()) / static_cast<double>(rowHeight)));
            notes = originalNotes;
            for (MidiNote& note : notes)
            {
                if (!selected.contains(note.id))
                    continue;
                note.startBeat = std::clamp(note.startBeat + deltaBeat, 0.0,
                                            std::max(0.0, patternLength - note.lengthBeats));
                note.pitch = std::clamp(note.pitch + deltaPitch, 0, 127);
            }
        }
        else if (dragMode == DragMode::Resize || dragMode == DragMode::Create)
        {
            const double endBeat = snapNearest(beatAt(point.x()));
            const double minimumLength = std::min(snap, patternLength);
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
            dragMode = DragMode::None;
            commitIfChanged(beforeEdit);
            update();
        }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
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
            update();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    enum class DragMode { None, Create, Move, Resize, Velocity };

    void updateCanvasSize()
    {
        const int width = sidebarWidth + static_cast<int>(std::ceil(patternLength * pixelsPerBeat)) + 40;
        const int height = rulerHeight + pitchCount * rowHeight + velocityHeight;
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
        return std::clamp(highestPitch - row, 0, 127);
    }

    double snapDown(double beat) const
    {
        return std::floor(beat / snap) * snap;
    }

    double snapNearest(double beat) const
    {
        return std::round(beat / snap) * snap;
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

    MidiEditor* owner;
    QVector<MidiNote> notes;
    QVector<MidiNote> beforeEdit;
    QVector<MidiNote> originalNotes;
    QSet<quint64> selected;
    QPoint dragStart;
    DragMode dragMode = DragMode::None;
    quint64 draggedNoteId = 0;
    double patternLength = 16.0;
    double snap = 0.25;
    double playheadBeat = -1.0;
    int pixelsPerBeat = 84;
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

void MidiEditor::setPatternLengthBeats(double beats)
{
    canvas->setLengthBeats(beats);
}

void MidiEditor::setSnap(double beatStep)
{
    canvas->setSnap(beatStep);
}

void MidiEditor::setPixelsPerBeat(int pixels)
{
    canvas->setPixelsPerBeat(pixels);
}

void MidiEditor::setPlayheadBeat(double beat)
{
    canvas->setPlayheadBeat(beat);
}
